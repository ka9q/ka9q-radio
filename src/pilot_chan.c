// radiod glue for the TS-1 pilot detector: the block anchor every linear
// channel records, the detector's output counter, INPUT_EPOCH, the worker
// thread that finishes folds, and naming the edge's UTC second.
//
// Threads. pilot_on_block runs in the channel's demod thread, once per block,
// and must never stall it: radiod's channel ring holds only a few blocks.
// So the demod thread only accumulates (pilot_accumulate, cheap per sample);
// at each fold end it wakes this channel's worker, which runs pilot_finish
// (~0.1 s at 96 kHz) at normal priority and posts the estimate. The demod
// thread picks the estimate up on a later block with a trylock, converts it
// to the input counter and names the second. chan->pilot.last and
// chan->anchor are written only by the demod thread, which also sends the
// channel's status (response() in radio.c), so status sees them consistent.
//
// The detector's output counter: (filter.out.sample_index - base) / decim,
// where base is the input sample index of the first block of the current
// epoch and decim = input samprate / output samprate (an integer, or the
// pilot stays off). The counter comes from the input sample index rather than
// the RTP timestamp because a block the demod thread misses advances the
// former but not the latter; the detector sees the skip as a gap and restarts
// its fold.
//
// Copyright 2026, HamSCI
#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "misc.h"
#include "radio.h"
#include "rtp.h"
#include "sched.h"
#include "timing.h"
#include "pilot.h"
#include "pilot_chan.h"

#define PILOT_SEARCH_WINDOW_MS 6.0 // hf-timestd search_window_ms

struct pilot_worker {
  pthread_t thread;
  pthread_mutex_t lock;
  pthread_cond_t cond;
  struct pilot_state *det;
  bool stop;
  // A job: the fold waiting in the detector's pending slot, tagged with the
  // counter it was folded on
  bool job;
  uint64_t job_epoch;
  int64_t job_base;
  // A result, for the demod thread
  bool ready;
  struct pilot_estimate est;
  uint64_t est_epoch;
  int64_t est_base;
  // Demod thread only
  int decim;               // input samples per output sample
  bool based;              // base holds a value for the current epoch
  int64_t base;            // input sample index of output counter 0
  uint64_t dropped_logged;
};

static void *pilot_worker_thread(void *arg){
  struct pilot_worker * const w = arg;
  pthread_setname("pilot");
  norealtime(); // finishing a fold may take 0.1 s; never at the demod's realtime priority
  pthread_mutex_lock(&w->lock);
  while(true){
    while(!w->job && !w->stop)
      pthread_cond_wait(&w->cond,&w->lock);
    if(w->stop)
      break;
    w->job = false;
    uint64_t const epoch = w->job_epoch;
    int64_t const base = w->job_base;
    pthread_mutex_unlock(&w->lock);

    struct pilot_estimate est;
    int const r = pilot_finish(w->det,&est);

    pthread_mutex_lock(&w->lock);
    if(r == 1){
      w->est = est;
      w->est_epoch = epoch;
      w->est_base = base;
      w->ready = true;
    }
  }
  pthread_mutex_unlock(&w->lock);
  return NULL;
}

// Set up the detector and its worker on the first block. False = can't run;
// the caller turns pilot off for this channel (until the next demod restart)
static bool pilot_chan_start(chan_t * const chan){
  struct frontend const * const frontend = chan->frontend;
  if(chan->filter2.blocking != 0){
    fprintf(stderr,"%s pilot: off; needs filter2 disabled (baseband must come from the channel filter)\n",chan->name);
    return false;
  }
  int const samprate = chan->output.samprate;
  double const ratio = samprate > 0 ? frontend->samprate / samprate : 0;
  if(!(ratio >= 1) || ratio != floor(ratio) || ratio > 1e9){
    fprintf(stderr,"%s pilot: off; input rate %.1f Hz / output rate %d Hz is not an integer\n",
	    chan->name,frontend->samprate,samprate);
    return false;
  }
  struct pilot_config const cfg = {
    .samprate = samprate,
    .fold_seconds = chan->pilot.fold_seconds,
    .search_window_ms = PILOT_SEARCH_WINDOW_MS,
    .snr_floor_db = chan->pilot.snr_floor,
  };
  struct pilot_state * const det = pilot_create(&cfg);
  if(det == NULL){
    fprintf(stderr,"%s pilot: off; bad config: rate %d Hz, fold %d s\n",chan->name,cfg.samprate,cfg.fold_seconds);
    return false;
  }
  struct pilot_worker * const w = calloc(1,sizeof(*w));
  assert(w != NULL);
  if(w == NULL){
    pilot_destroy(det);
    return false;
  }
  w->det = det;
  w->decim = (int)ratio;
  pthread_mutex_init(&w->lock,NULL);
  pthread_cond_init(&w->cond,NULL);
  if(pthread_create(&w->thread,NULL,pilot_worker_thread,w) != 0){
    fprintf(stderr,"%s pilot: off; can't start worker thread\n",chan->name);
    pthread_mutex_destroy(&w->lock);
    pthread_cond_destroy(&w->cond);
    pilot_destroy(det);
    free(w);
    return false;
  }
  chan->pilot.det = det;
  chan->pilot.worker = w;
  memset(&chan->pilot.last,0,sizeof(chan->pilot.last));
  fprintf(stderr,"%s pilot: on; fold %d s, decimation %d, SNR floor %.1f dB\n",
	  chan->name,cfg.fold_seconds,w->decim,cfg.snr_floor_db);
  return true;
}

void pilot_chan_stop(chan_t * const chan){
  struct pilot_worker * const w = chan->pilot.worker;
  if(w != NULL){
    pthread_mutex_lock(&w->lock);
    w->stop = true;
    pthread_cond_signal(&w->cond);
    pthread_mutex_unlock(&w->lock);
    pthread_join(w->thread,NULL); // waits out a fold being finished, at most ~0.1 s
    pthread_mutex_destroy(&w->lock);
    pthread_cond_destroy(&w->cond);
    free(w);
  }
  pilot_destroy(chan->pilot.det);
  chan->pilot.worker = NULL;
  chan->pilot.det = NULL;
  chan->pilot.last.valid = false;
}

// Copy the front end's valid epoch marks (see input_epoch_begin): a slot
// counts only if its epoch reads the same, and nonzero, before and after start
static int read_epoch_marks(struct frontend * const frontend,struct pilot_epoch_mark *marks){
  int n = 0;
  for(int i=0; i < PILOT_EPOCH_MARKS; i++){
    uint64_t const e1 = atomic_load(&frontend->epoch_marks[i].epoch);
    int64_t const start = atomic_load(&frontend->epoch_marks[i].start);
    uint64_t const e2 = atomic_load(&frontend->epoch_marks[i].epoch);
    if(e1 != 0 && e1 == e2){
      marks[n].epoch = e1;
      marks[n].start = start;
      n++;
    }
  }
  return n;
}

// Group delay of the channel filter, seconds. set_filter() (filter.c:1047-1079)
// builds a Kaiser-windowed sinc of M = points - olen + 1 taps in the first M
// time-domain points, centred at (M-1)/2 and shifted to the passband centre by
// cispi(2 center n) with n counted from that centre: linear phase, not zero
// phase. Overlap-save keeps the last olen points of each inverse FFT
// (filter.c:400, the first M-1 discarded at :973-977), and new input lands at
// the end of each forward FFT window (filter.c:281,295), so output sample k
// of a block responds to input around k - (M-1)/2: the edge appears
// (M-1)/2 output samples late. Reported beside the raw edge, never subtracted
static double filter_delay_s(chan_t const * const chan){
  struct filter_out const * const f = &chan->filter.out;
  if(chan->output.samprate <= 0 || f->points <= f->olen)
    return 0;
  return 0.5 * (f->points - f->olen) / chan->output.samprate;
}

void pilot_on_block(chan_t * const chan){
  assert(chan != NULL);
  struct frontend * const frontend = chan->frontend;
  int64_t const sample_index = (int64_t)chan->filter.out.sample_index;

  // A lapped-ring block: execute_filter_output() (filter.c:752-761) returns
  // zeros without updating sample_index, while send_output() still advances
  // the RTP timestamp for it. Its index says nothing about its RTP, so keep the
  // last good anchor and keep the zeros from the detector. A forward jump is a
  // real block (skipped jobs, or the block after a zero block) with its own index
  bool const fresh = pilot_block_fresh(chan->anchor.seen,chan->anchor.block_index,sample_index);
  chan->anchor.block_index = sample_index;
  chan->anchor.seen = true;
  if(!fresh)
    return;

  // This block's epoch. Losses are marked at the input sample where they
  // occurred, and the demod thread runs a block or two behind the driver, so
  // a new epoch takes effect at the first block that starts at or after its
  // mark, not when input_epoch changes. A block with a mark inside it keeps
  // its first sample's epoch but holds both sides of the loss: it stays out of
  // the detector, whose next block then arrives as a gap
  uint64_t epoch = chan->anchor.epoch;
  bool straddle = false;
  if(atomic_load_explicit(&frontend->input_epoch,memory_order_acquire) != epoch){
    struct pilot_epoch_mark marks[PILOT_EPOCH_MARKS];
    int const n = read_epoch_marks(frontend,marks);
    straddle = pilot_epoch_for_block(marks,n,epoch,sample_index,frontend->in.ilen,&epoch) != 0;
  }

  // The anchor. send_output() stamps each packet with chan->output.rtp.timestamp
  // (audio.c:78) and then advances it by the frames sent (audio.c:184-187);
  // frames it can't send yet wait in chan->output.queue (queue_length counts
  // frames: audio.c:100-107, 236-242) and go out ahead of new data
  // (audio.c:89-114). Here, before this block's send_output() (linear.c:314),
  // the timestamp belongs to the oldest queued frame, so this block's first
  // sample gets timestamp + queue_length, scaled to Opus' fixed 48 kHz clock
  // as audio.c:185 does. With buffer = 0 (maxdelay 0) the queue is always
  // empty after send_output and this is the bare timestamp. Caveats from
  // Phil's code, unchanged: a squelch-tail block (linear.c:288, buffer NULL)
  // does not advance the timestamp, and a muted block (linear.c:292,
  // audio.c:48-52) advances it without draining the queue
  uint32_t queued = (uint32_t)chan->output.queue_length;
  if((chan->output.encoding == OPUS || chan->output.encoding == OPUS_VOIP) && chan->output.samprate > 0)
    queued = (uint32_t)((int64_t)queued * OPUS_SAMPRATE / chan->output.samprate);
  chan->anchor.rtp = chan->output.rtp.timestamp + queued;
  chan->anchor.input_sample = sample_index;
  chan->anchor.epoch = epoch;

  if(!chan->pilot.enable){
    if(chan->pilot.worker != NULL)
      pilot_chan_stop(chan); // a client switched the pilot off (PILOT_ENABLE)
    return;
  }
  if(chan->baseband == NULL || chan->sampcount <= 0)
    return;
  if(chan->pilot.worker == NULL && !pilot_chan_start(chan)){
    chan->pilot.enable = false;
    return;
  }
  struct pilot_worker * const w = chan->pilot.worker;
  struct pilot_state * const det = chan->pilot.det;

  // A new epoch: forget everything and rebase the output counter here
  if(pilot_epoch_check(det,&chan->pilot.last,&chan->pilot.epoch_seen,epoch) || !w->based){
    w->base = sample_index;
    w->based = true;
  }
  int64_t const d = sample_index - w->base;
  if(d < 0 || d % w->decim != 0){
    // Off the output grid (can't happen with radiod's integer block ratio): rebase
    pilot_forget(det);
    chan->pilot.last.valid = false;
    w->base = sample_index;
  }
  int const flags = straddle ? 0 :
    pilot_accumulate(det,chan->baseband,chan->sampcount,(sample_index - w->base) / w->decim);
  if(flags > 0 && (flags & PILOT_FOLD)){
    pthread_mutex_lock(&w->lock);
    w->job = true;
    w->job_epoch = chan->pilot.epoch_seen;
    w->job_base = w->base;
    pthread_cond_signal(&w->cond);
    pthread_mutex_unlock(&w->lock);
  }
  uint64_t const dropped = pilot_dropped(det);
  if(dropped != w->dropped_logged){
    // A fold completed before the worker finished the previous one: should never happen
    fprintf(stderr,"%s pilot: worker behind, %llu fold(s) dropped\n",chan->name,(unsigned long long)dropped);
    w->dropped_logged = dropped;
  }

  // Collect a finished fold, if the worker posted one; never wait for it
  if(pthread_mutex_trylock(&w->lock) != 0)
    return;
  bool const ready = w->ready;
  struct pilot_estimate est;
  uint64_t est_epoch = 0;
  int64_t est_base = 0;
  if(ready){
    est = w->est;
    est_epoch = w->est_epoch;
    est_base = w->est_base;
    w->ready = false;
  }
  pthread_mutex_unlock(&w->lock);
  if(!ready || est_epoch != chan->pilot.epoch_seen)
    return; // nothing new, or folded on a counter that has since rebased

  // Name the second: the host clock now, carried back to the edge by the
  // input samples counted since it. frontend->samples counts every sample
  // that has arrived (rx888.c rx_callback), so it pairs with gps_time_ns()
  // read beside it; USB and scheduling latency (~ms) stay in host_time_ns,
  // far inside the half second the rounding allows
  int64_t const now_input = (int64_t)frontend->samples;
  int64_t const now_ns = gps_time_ns();
  pilot_make_report(&chan->pilot.last,&est,est_epoch,est_base,w->decim,chan->output.samprate,
		    frontend->samprate,now_ns,now_input,filter_delay_s(chan));
}
