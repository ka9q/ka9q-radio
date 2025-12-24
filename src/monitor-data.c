// Data plane sections of the multicast monitor program
// Moved out of monitor.c when it was getting way too big
// Copyright Aug 2024 Phil Karn, KA9Q

#define _GNU_SOURCE 1
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include <opus/opus.h>
#include <portaudio.h>
#include <ncurses.h>
#include <locale.h>
#include <signal.h>
#include <getopt.h>
#include <iniparser/iniparser.h>
#include <samplerate.h>
#if __linux__
#include <bsd/string.h>
#include <alsa/asoundlib.h>
#else
#include <string.h>
#endif
#include <sysexits.h>
#include <poll.h>
#include <stdatomic.h>

#include "conf.h"
#include "config.h"
#include "misc.h"
#include "multicast.h"
#include "radio.h"
#include "iir.h"
#include "morse.h"
#include "status.h"
#include "monitor.h"

#define DATA_PRIORITY 50

int Position; // auto-position streams
int Invalids;

// All the tones from various groups, including special NATO 150 Hz tone
double PL_tones[] = {
     67.0,  69.3,  71.9,  74.4,  77.0,  79.7,  82.5,  85.4,  88.5,  91.5,
     94.8,  97.4, 100.0, 103.5, 107.2, 110.9, 114.8, 118.8, 123.0, 127.3,
    131.8, 136.5, 141.3, 146.2, 150.0, 151.4, 156.7, 159.8, 162.2, 165.5,
    167.9, 171.3, 173.8, 177.3, 179.9, 183.5, 186.2, 189.9, 192.8, 196.6,
    199.5, 203.5, 206.5, 210.7, 213.8, 218.1, 221.3, 225.7, 229.1, 233.6,
    237.1, 241.8, 245.5, 250.3, 254.1
};
static double make_position(int x);
static bool legal_opus_size(int n);
static void reset_playout(struct session *sp);
static void impatient_wait(struct session *sp, int64_t const ns);
static void init_pl(struct session *sp);
static int run_pl(struct session *sp);
static void apply_notch(struct session *sp);
static int conceal_or_wait(struct session *sp);
static int decode_rtp_data(struct session *sp,struct packet const *pkt);
static void copy_to_stream(struct session *sp);
static int upsample(struct session *sp);


// Receive from data multicast streams, multiplex to decoder threads
void *dataproc(void *arg){
  char const *mcast_address_text = (char *)arg;
  {
    char name[100];
    snprintf(name,sizeof(name),"mon %s",mcast_address_text);
    pthread_setname(name);
  }

  int input_fd;
  {
    char iface[1024];
    struct sockaddr group = {0};

    resolve_mcast(mcast_address_text,&group,DEFAULT_RTP_PORT,iface,sizeof(iface),0);
    input_fd = listen_mcast(Source_socket,&group,iface);
  }
  if(input_fd == -1)
    pthread_exit(NULL);

  struct packet *pkt = NULL;

  realtime(DATA_PRIORITY);
  // Main loop begins here
  while(!Terminate){
    // Need a new packet buffer?
    if(!pkt)
      pkt = malloc(sizeof(*pkt));
    // Zero these out to catch any uninitialized derefs
    pkt->next = NULL;
    pkt->data = NULL;
    pkt->len = 0;

    struct sockaddr_storage sender;
    socklen_t socksize = sizeof(sender);
    ssize_t size = recvfrom(input_fd,&pkt->content,sizeof(pkt->content),0,(struct sockaddr *)&sender,&socksize);
    if(size == -1){
      if(errno != EINTR){ // Happens routinely, e.g., when window resized
	perror("recvfrom");
	usleep(1000);
      }
      continue;  // Reuse current buffer
    }
    if(size <= RTP_MIN_SIZE)
      continue; // Must be big enough for RTP header and at least some data

    // Convert RTP header to host format
    uint8_t const *dp = ntoh_rtp(&pkt->rtp,pkt->content);
    pkt->data = dp;
    pkt->len = size - (dp - pkt->content);
    if(pkt->rtp.pad){
      pkt->len -= dp[pkt->len-1];
      pkt->rtp.pad = 0;
    }
    if(pkt->len <= 0)
      continue; // Used to be an assert, but would be triggered by bogus packets

    kick_output(); // Ensure output thread is running
    // Find appropriate session; create new one if necessary
    struct session *sp = lookup_or_create_session(&sender,pkt->rtp.ssrc);
    if(!sp){
      fprintf(stderr,"No room!!\n");
      continue;
    }
    if(!sp->init){
      // status reception doesn't write below this point
      if(Auto_position)
	sp->pan = make_position(Position++);
      else
	sp->pan = 0;     // center by default
      sp->gain = dB2voltage(Gain);    // Start with global default
      sp->notch_enable = Notch;
      sp->muted = Start_muted;
      sp->dest = mcast_address_text;
      sp->rtp_state.timestamp = sp->next_timestamp = pkt->rtp.timestamp;
      sp->rtp_state.seq = pkt->rtp.seq;
      sp->reset = true;
      sp->init = true;

      if(pthread_create(&sp->task,NULL,decode_task,sp) == -1){
	perror("pthread_create");
	close_session(sp);
	continue;
      }
    }
    sp->packets++;
    sp->last_active = gps_time_ns();
    // Discard packets with unknown encoding
    // This will happen before the first status arrives
    enum encoding const encoding = sp->pt_table[sp->type].encoding;
    if(encoding == NO_ENCODING || encoding == AX25)
      continue;

    // Insert onto queue sorted by sequence number, wake up thread
    struct packet *q_prev = NULL;
    struct packet *qe = NULL;

    int qlen = 0;
    const int maxq = 500; // 10 seconds
    pthread_mutex_lock(&sp->qmutex);
    for(qe = sp->queue;
	qe != NULL && qlen < maxq && pkt->rtp.seq >= qe->rtp.seq;
	q_prev = qe,qe = qe->next,qlen++)
      ;

    if(qlen >= maxq){
      // Queue has grown huge, blow it away. Seems to happen when a macos laptop is asleep
      struct packet *qnext;
      for(qe = sp->queue; qe != NULL; qe = qnext){
	qnext = qe->next;
	FREE(qe);
      }
      q_prev = NULL;
      // queue now empty, can put new packet at head
    }
    if(qe)
      sp->reseqs++;   // Not the last on the list
    pkt->next = qe;
    if(q_prev)
      q_prev->next = pkt;
    else
      sp->queue = pkt; // Front of list
    pkt = NULL;        // force new packet to be allocated

    // wake up decoder thread
    pthread_cond_signal(&sp->qcond);
    pthread_mutex_unlock(&sp->qmutex);
  }
  return NULL;
}
void decode_task_cleanup(void *arg){
  struct session *sp = (struct session *)arg;
  assert(sp);

  ASSERT_UNLOCKED(&sp->qmutex);
  pthread_mutex_destroy(&sp->qmutex);
  pthread_cond_destroy(&sp->qcond);

  if(sp->opus){
    opus_decoder_destroy(sp->opus);
    sp->opus = NULL;
  }
  struct packet *pkt_next = NULL;
  for(struct packet *pkt = sp->queue; pkt != NULL; pkt = pkt_next){
    pkt_next = pkt->next;
    FREE(pkt);
  }
  if(sp->src_state_mono)
    src_delete(sp->src_state_mono);
  sp->src_state_mono = NULL;
  if(sp->src_state_stereo)
    src_delete(sp->src_state_stereo);
  sp->src_state_mono = NULL;

  // Just in case anything was allocated for these arrays
  struct channel * const chan = &sp->chan;
  FREE(chan->spectrum.bin_data);
  FREE(chan->status.command);
  FREE(chan->spectrum.window);
  FREE(chan->spectrum.plan);
  FREE(chan->spectrum.ring);
  memset((void *)sp,0,sizeof(*sp)); // blow it all away
}

// Per-session thread to decode incoming RTP packets
// Not needed for PCM, but Opus can be slow
void *decode_task(void *arg){
  struct session * const sp = (struct session *)arg;
  assert(sp);
  {
    char name[100];
    snprintf(name,sizeof(name),"dec %u",sp->ssrc);
    pthread_setname(name);
  }
  pthread_cleanup_push(decode_task_cleanup,arg); // called on termination
  struct packet *pkt = NULL; // make sure it starts this way
  reset_playout(sp);
  bool init = false;
  // Main loop; run until asked to quit
  while(!sp->terminate && !Terminate){
    if(sp->reset) // Requested by somebody
      reset_playout(sp);
    assert(pkt == NULL); // should be cleared, otherwise it's a memory leak
    // The queue is sorted by sequence number. Anything on it?
    pthread_mutex_lock(&sp->qmutex);
    if((pkt = sp->queue) == NULL){
      pthread_mutex_unlock(&sp->qmutex);
      conceal_or_wait(sp);
    } else {
      // pkt != NULL
      if(!init){
	// Very first packet, accept it
	sp->rtp_state.seq = pkt->rtp.seq; // expect the first sequence number
	sp->next_timestamp = pkt->rtp.timestamp; // and timestamp
	init = true;
      }
      if(pkt->rtp.seq == sp->rtp_state.seq){
	// Common fast path, holding lock
	sp->queue = sp->queue->next;
	pthread_mutex_unlock(&sp->qmutex);
	pkt->next = NULL;
	decode_rtp_data(sp,pkt);
	sp->rtp_state.seq = pkt->rtp.seq + 1; // Expect the one after this next
	FREE(pkt);
      } else if(++sp->consec_out_of_order < 6){
	// Still holding lock
	int seq_diff = (int16_t)(pkt->rtp.seq - sp->rtp_state.seq);
	assert(seq_diff != 0);
	// old or future?
	if(seq_diff < 0){
	  // Old duplicate, discard and look again
	  sp->queue = sp->queue->next;
	  pthread_mutex_unlock(&sp->qmutex);
	  FREE(pkt);
	  sp->rtp_state.drops++;
	  Last_error_time = gps_time_ns();
	  continue; // Go back for another packet
	} else {
	  // seq_diff > 0; Hold for reassembly
	  pthread_mutex_unlock(&sp->qmutex);
	  conceal_or_wait(sp);
	}
      } else {
	// sp->consec_out_of_order >= 6, assume a restart
	// resynch ("Vhat are you resynching about?")
	sp->queue = sp->queue->next;
	pthread_mutex_unlock(&sp->qmutex);
	pkt->next = NULL;
	reset_playout(sp);
	sp->next_timestamp = pkt->rtp.timestamp;
	sp->consec_out_of_order = 0;
	decode_rtp_data(sp,pkt);
	sp->rtp_state.seq = pkt->rtp.seq + 1; // Expect the one after this next
	FREE(pkt);
      }
    } // pkt != NULL
    // Rest of packet processing
    // All if/else clauses above should reach here except old sequence duplicates
    // Do PL detection and notching even when muted or outvoted
    if(sp->samprate == 0 || sp->channels == 0||sp->frame_size == 0)
      continue; // Not yet fully initialized
    if(sp->notch_enable){
      run_pl(sp);
      apply_notch(sp);
    }
    if(!sp->muted && (!Voting || Best_session == sp)){
      upsample(sp);
      copy_to_stream(sp);
    }
  } // !sp->terminate
  pthread_cleanup_pop(1);
  return NULL;
}

// Reset playout buffer
// also reset Opus decoder, if present
static void reset_playout(struct session * const sp){
  sp->resets++;
  if(sp->opus)
    opus_decoder_ctl(sp->opus,OPUS_RESET_STATE); // Reset decoder
  sp->reset = false;
  sp->playout = (int)round(Playout * DAC_samprate);

  int64_t const rptr = atomic_load_explicit(&Output_time,memory_order_relaxed);
  atomic_store_explicit(&sp->wptr,rptr + sp->playout,memory_order_release);
}
// Start output stream if it was off; reset idle timeout on output audio stream activity
// Return true if we (re)started it
bool kick_output(void){
  bool restarted = false;
  if(!Pipe && !Pa_IsStreamActive(Pa_Stream)){
    // Start it up
    if(!Pa_IsStreamStopped(Pa_Stream))
      Pa_StopStream(Pa_Stream); // it was in limbo

    Start_time = gps_time_ns();
    Start_pa_time = Pa_GetStreamTime(Pa_Stream); // Stream Time runs continuously even when stream stopped
    atomic_store_explicit(&Audio_frames,0,memory_order_relaxed);
    int r = Pa_StartStream(Pa_Stream); // Immediately triggers the first callback
    if(r != paNoError){
      fprintf(stderr,"Portaudio error: %s, aborting\n",Pa_GetErrorText(r));
      abort();
    }
    for(int i=0; i < NSESSIONS; i++){
      if(!Sessions[i].init)
	continue;
      reset_playout(&Sessions[i]);
    }

    restarted = true;
  }
  // Key up the repeater if it's configured and not already on
  if(Repeater_tail != 0){
    LastAudioTime = gps_time_ns();
    pthread_mutex_lock(&PTT_mutex);
    if(!PTT_state){
      PTT_state = true;
      pthread_cond_signal(&PTT_cond); // Notify the repeater control thread to ID and run drop timer
    }
    pthread_mutex_unlock(&PTT_mutex);
  }
  return restarted;
}
// Assign pan position by reversing binary bits of counter
// Returns -1 to +1
static double make_position(int x){
  x += 1; // Force first position to be in center, which is the default with a single stream
  // Swap bit order
  int y = 0;
  const int w = 8;
  for(int i=0; i < w; i++){
    y = (y << 1) | (x & 1);
    x >>= 1;
  }
  // Scale
  return 0.5 * (((double)y / 128) - 1);
}
static bool legal_opus_size(int n){
  // 2.5, 5, 10, 20, 40, 60, 80, 100, 120
  if(n == 120 || n == 240 || n == 480 || n == 960 || n == 1920 || n == 2880 || n == 3840 || n == 4800 || n == 5760)
    return true;
  return false;
}

// Sleep ns nanoseconds or until a packet arrives
static void impatient_wait(struct session *sp, int64_t const ns){
  struct timespec deadline = {0};
  if(clock_gettime(CLOCK_REALTIME,&deadline) != 0){
    fprintf(stderr,"clock_gettime(CLOCK_REALTIME): %s\n",strerror(errno));
    assert(false);
  }
  deadline.tv_nsec += ns % BILLION;
  deadline.tv_sec += ns / BILLION;
  pthread_mutex_lock(&sp->qmutex);
  pthread_cond_timedwait(&sp->qcond,&sp->qmutex,&deadline);
  pthread_mutex_unlock(&sp->qmutex);
}

// Extract the data from an incoming RTP, place in bounce buffer
// Decode opus or just convert PCM
// Returns number of bytes written into bounce buffer, which may be 0
// caller may then free packet
static int decode_rtp_data(struct session *sp,struct packet const *pkt){
  assert(sp != NULL && pkt != NULL);
  if(sp == NULL || pkt == NULL)
    return 0;
  sp->type = pkt->rtp.type;
  int prev_samprate = sp->samprate;
  if(sp->pt_table[sp->type].encoding == OPUS){
    // The table values reflect the encoder input; they're for the status display
    // The encoder output is always forced to the local DAC
    sp->samprate = DAC_samprate;
    sp->channels = Channels;
  } else {
    // Use actual table values for PCM
    sp->samprate = sp->pt_table[sp->type].samprate;
    sp->channels = sp->pt_table[sp->type].channels;
  }
  if(sp->samprate != prev_samprate)
    init_pl(sp);
  if(sp->samprate == 0){
    // Don't know the sample rate yet, we can't proceed
    sp->rtp_state.drops++; // may cause big burst at start of stream
    return 0;
  }
  if(pkt->rtp.marker){ // burst start
    reset_playout(sp);
    sp->next_timestamp = pkt->rtp.timestamp;
  }
  sp->consec_erasures = 0;
  sp->rtp_state.timestamp = pkt->rtp.timestamp; // used for delay calcs
  if(pkt->len <= 0){
    sp->frame_size = 0;
    sp->empties++;
    sp->rtp_state.drops++;
    return 0;
  }
  // This section processes the signal in the current RTP frame, copying and/or decoding it into a sp->bounce buffer
  // for mixing with the output ring buffer
  if(sp->pt_table[sp->type].encoding == OPUS){
    // The Opus decoder output is always forced to the local channel count and sample rate
    // The values in the table reflect the *encoder input*
    if(!sp->opus){
      // This should happen only once on a stream
      // Always decode Opus to DAC rate of 48 kHz, stereo
      int error;
      sp->opus = opus_decoder_create(DAC_samprate,Channels,&error);
      if(error != OPUS_OK)
	fprintf(stderr,"opus_decoder_create error %d\n",error);

      assert(sp->opus);
      opus_decoder_ctl(sp->opus,OPUS_SET_COMPLEXITY(10)); // Turn on all the new cool stuff

    }
    opus_int32 const r1 = opus_packet_get_nb_samples(pkt->data,(opus_int32)pkt->len,sp->samprate);
    if(r1 == OPUS_INVALID_PACKET || r1 == OPUS_BAD_ARG){
      sp->rtp_state.drops++;
      return -2;
    }
    assert(r1 >= 0);
    sp->frame_size = r1;
    opus_int32 const r2 = opus_packet_get_bandwidth(pkt->data);
    sp->bandwidth = opus_bandwidth(NULL,r2);
    sp->opus_channels = opus_packet_get_nb_channels(pkt->data); // Only for display purposes. We always decode to output preference
    // by 'samples' they apparently mean stereo samples
    opus_int32 const decoded_samples = opus_decode_float(sp->opus,pkt->data,(opus_int32)pkt->len,
							 sp->bounce,
							 (int)sp->frame_size,0);
    assert(decoded_samples == (opus_int32)sp->frame_size); // Or something is broken inside Opus
    // Maintain smoothed measurement of data rate
    // Won't work right with discontinuous transmission - fix by looking at timestamps
    double const rate = 8 * pkt->len * DAC_samprate / (double)decoded_samples; // 8 bits/byte * length / (samples/samprate)
    sp->datarate += 0.1 * (rate - sp->datarate);
    return 0;
  }
  switch(sp->pt_table[sp->type].encoding){
  case S16BE:
    sp->datarate = 8 * sp->channels * sizeof(int16_t) * sp->samprate; // fixed rate
    sp->frame_size = pkt->len / (sizeof(int16_t) * sp->channels); // mono/stereo samples in frame
    {
      int16_t const * const data = (int16_t *)&pkt->data[0];
      for(int i=0; i < sp->channels * sp->frame_size; i++)
	sp->bounce[i] = SCALE16 * (int16_t)ntohs(data[i]); // Cast is necessary
    }
    break;
  case S16LE: // same as S16BE but no byte swap - assumes little-endian machine
    sp->datarate = 8 * sp->channels * sizeof(int16_t) * sp->samprate; // fixed rate
    sp->frame_size = pkt->len / (sizeof(int16_t) * sp->channels); // mono/stereo samples in frame
    {
      int16_t const * const data = (int16_t *)&pkt->data[0];
      for(int i=0; i < sp->channels * sp->frame_size; i++)
	sp->bounce[i] = SCALE16 * data[i];
    }
    break;
  case F32LE:
    sp->datarate = 8 * sp->channels * sizeof(float) * sp->samprate;
    sp->frame_size = pkt->len / (sizeof(float) * sp->channels); // mono/stereo samples in frame
    {
      float const * const data = (float *)&pkt->data[0];
      for(int i=0; i < sp->channels * sp->frame_size; i++)
	sp->bounce[i] = data[i];
    }
    break;
#ifdef HAS_FLOAT16
  case F16LE: // 16-bit floats
    sp->datarate = 8 * sp->channels * sizeof(float16_t) * sp->samprate;
    sp->frame_size = pkt->len / (sizeof(float16_t) * sp->channels); // mono/stereo samples in frame
    {
      float16_t const * const data = (float16_t *)&pkt->data[0];
      for(int i=0; i < sp->channels * sp->frame_size; i++)
	sp->bounce[i] = data[i];
    }
    break;
#endif
  default:
    sp->rtp_state.drops++;
    sp->frame_size = 0;
    return -2; // No change to next_timestamp or wptr because we don't know the sample size
  } // end of PCM switch
  return 0;
}

// Called when there isn't an in-sequence packet to be processed
static int conceal_or_wait(struct session *sp){
  int64_t const rptr = atomic_load_explicit(&Output_time,memory_order_relaxed);
  int64_t const wptr = atomic_load_explicit(&sp->wptr,memory_order_relaxed);
  // Read and write pointers and sample rate  must be initialized to be useful
  if(rptr == 0 || wptr == 0 || sp->samprate == 0){
    impatient_wait(sp,BILLION/10); // fixed 100 ms
    return 0;
  }
  // wptr and rptr are in frames
  int64_t margin = wptr - (rptr + sp->playout);
  if(margin > 0){
    // Playout queue is happy, we don't need to do anything right now
    // Probably shouldn't wait until the last microsecond, decrease this a little
    int64_t duration = BILLION * margin / sp->samprate;
    if(duration > BILLION)
      duration = BILLION; // guard against random values
    impatient_wait(sp,duration);
    sp->frame_size = 0; // don't emit anything
    return 0; // look again
  }

  // Time has run out, so we gotta do something; generate some PLC or silence
  int seq_diff = 0;
  int time_diff = 0;
  // Peek at the queue and find the time jump to the next waiting packet, if any
  pthread_mutex_lock(&sp->qmutex);
  struct packet const *pkt = sp->queue;
  if(pkt != NULL){
    time_diff = (int32_t)(pkt->rtp.timestamp - sp->next_timestamp);
    seq_diff = (int16_t)(pkt->rtp.seq - sp->rtp_state.seq);
  }
  pthread_mutex_unlock(&sp->qmutex);

  // If seq_diff == 0, the queue is empty.
  // if seq_diff > 0, there's a gap and we've noted seq_diff and time_diff
  // This is not necessarily a lost packet, the stream might have stopped
  sp->frame_size = (int)round(sp->samprate * .02); // fake a frame of this length
  // time_diff is nonzero only if there's a gap before the next packet on the queue
  assert(seq_diff >= 0);
  if(seq_diff != 0 && time_diff > 0 && sp->frame_size > time_diff)
    sp->frame_size = time_diff; // But limit to the gap, if we know what it is

  if(sp->opus && legal_opus_size(sp->frame_size) && ++sp->consec_erasures <= 3){
    // Trigger loss concealment, up to 3 consecutive packets
    sp->plcs++;
    opus_int32 const frame_count = opus_decode_float(sp->opus,NULL,0,
					       sp->bounce,sp->frame_size,0);
    (void)frame_count;
    assert(frame-count == sp->frame_size);
    return sp->frame_size;
  } else {
    memset(sp->bounce,0,sizeof(sp->bounce));
    return 0; // stopped
  }
}

static int run_pl(struct session *sp){
  assert(sp != NULL);
  if(sp == NULL)
    return -1;

  if(sp->samprate == 0)
    return -1;

  // Run PL tone decoders on the bounce buffer. We don't really care about timestamp jumps
  // Disable if display isn't active and autonotching is off
  // Fed audio that might be discontinuous or out of sequence, but it's a pain to fix
  for(int i=0; i < sp->frame_size; i++){
    double s;
    if(sp->channels == 2)
      s = 0.5 * sp->bounce[2*i] + sp->bounce[2*i+1];
    else
      s = sp->bounce[i];

    for(int j = 0; j < N_tones; j++)
      update_goertzel(&sp->tone_detector[j],s);
  }
  sp->tone_samples += sp->frame_size;
  if(sp->tone_samples >= Tone_period * sp->samprate){
    sp->tone_samples = 0;
    int pl_tone_index = -1;
    double strongest_tone_energy = 0;
    double total_energy = 0;
    for(int j=0; j < N_tones; j++){
      double const energy = cnrm(output_goertzel(&sp->tone_detector[j]));
      total_energy += energy;
      reset_goertzel(&sp->tone_detector[j]);
      if(energy > strongest_tone_energy){
	strongest_tone_energy = energy;
	pl_tone_index = j;
      }
    }
    if(2*strongest_tone_energy > total_energy && pl_tone_index >= 0){
      // Tone must be > -3dB relative to total of all tones
      sp->current_tone = PL_tones[pl_tone_index];
    } else
      sp->current_tone = 0;
  } // End of tone observation period
  if(sp->current_tone != 0 && sp->notch_tone != sp->current_tone){
    // New or changed tone
    sp->notch_tone = sp->current_tone;
    setIIRnotch(&sp->iir_right,sp->current_tone/sp->samprate);
    setIIRnotch(&sp->iir_left,sp->current_tone/sp->samprate);
  }
  return 0;
} // End of PL tone decoding

static void init_pl(struct session *sp){
  assert(sp != NULL);
  if(sp == NULL)
    return;

  // Init PL tone detectors
  for(int j=0; j < N_tones; j++)
    init_goertzel(&sp->tone_detector[j],PL_tones[j]/(double)sp->samprate);
  sp->notch_tone = 0;
}
static void apply_notch(struct session *sp){
  assert(sp != NULL);
  if(sp == NULL)
    return;

  // Apply notch filter, if enabled
  // Do this even when not selected by voting, to prevent transients when it's selected
  if(sp->notch_tone > 0){
    if(sp->channels == 1){
      for(int i = 0; i < sp->frame_size; i++)
	sp->bounce[i] = (float)applyIIR(&sp->iir_left,sp->bounce[i]);
    } else {
      for(int i = 0; i < sp->frame_size; i++){
	sp->bounce[2*i] = (float)applyIIR(&sp->iir_left,sp->bounce[2*i]);
	sp->bounce[2*i+1] = (float)applyIIR(&sp->iir_right,sp->bounce[2*i+1]);
      }
    }
  } // End of tone notching
}
// Upsample to DAC rate if necessary
static int upsample(struct session *sp){
  assert(sp != NULL && sp->samprate != 0 && sp->channels != 0);
  if(sp == NULL || sp->samprate == 0 || sp->channels == 0)
    return -1;

  if(sp->frame_size == 0)
    return 0;

  if(sp->samprate == DAC_samprate){
    // No conversion necessary
    sp->output_rate_data = sp->bounce;
    sp->output_rate_data_size = sp->frame_size;
    return sp->output_rate_data_size;
  }
  double upsample_ratio = DAC_samprate / sp->samprate;
  assert(sp->frame_size * upsample_ratio * sp->channels <= BBSIZE);
  if(sp->frame_size * upsample_ratio * sp->channels > BBSIZE)
    return -1;

  int error;
  if(sp->channels == 1 && sp->src_state_mono == NULL){
    sp->src_state_mono = src_new(SRC_SINC_FASTEST, 1, &error);
    assert(sp->src_state_mono != NULL);
  } else if(sp->channels == 2 && sp->src_state_stereo == NULL){
    sp->src_state_stereo = src_new(SRC_SINC_FASTEST, 2, &error);
    assert(sp->src_state_stereo != NULL);
  }
  SRC_DATA src_data = {0};
  src_data.data_in = sp->bounce;  // Pointer to input audio
  src_data.data_out = sp->rate_converted_buffer;  // Pointer to resampled output buffer
  src_data.input_frames = sp->frame_size;
  src_data.output_frames = (int)ceil(sp->frame_size * upsample_ratio + 1);
  src_data.src_ratio = upsample_ratio;
  src_data.end_of_input = 0;
  if(sp->channels == 1)
    error = src_process(sp->src_state_mono, &src_data);
  else
    error = src_process(sp->src_state_stereo, &src_data);
  assert(error == 0);
  sp->output_rate_data = sp->rate_converted_buffer;
  sp->output_rate_data_size = (int)round(sp->frame_size * upsample_ratio);
  return sp->output_rate_data_size;
}

static void copy_to_stream(struct session *sp){
  assert(sp != NULL);
  if(sp == NULL)
    return;

  // Figure out where to write into output buffer
  // We didn't actully know how many samples we have to write until after decoding
  //
  if(sp->output_rate_data_size == 0)
    return;

  int64_t const rptr = atomic_load_explicit(&Output_time,memory_order_relaxed); // frames
  int64_t wptr = atomic_load_explicit(&sp->wptr,memory_order_acquire); // frames

  int room = wptr - rptr;
  if(room < sp->output_rate_data_size){
    // oops not enough room; keep what fits
    if(room <= 0){
      // Nothing fits
      sp->lates++;
      if(++sp->consec_lates < 6){
	sp->rtp_state.drops++;
	return;
      }
      // A playout reset should make room
      sp->consec_lates = 0;
      reset_playout(sp);
    } else {
      // Keep what fits
      sp->output_rate_data += sp->output_rate_data_size - room;
      wptr += sp->output_rate_data_size - room;
      sp->output_rate_data_size = room;
    }
  }
  {
    // Measure output audio level

    int count = sp->output_rate_data_size;
    if(sp->channels == 2)
      count *= 2;

    if(count > 0){
      double energy = 0;
      for(int i=0; i < count; i++)
	energy += sp->output_rate_data[i] * sp->output_rate_data[i];
      
      energy /= count;
      if(isfinite(energy))
	sp->level += .01 * (energy - sp->level); // smooth
    }
  }
  // Mix output sample rate data into output ring buffer
  int64_t base = wptr * Channels; // Base of output buffer for this packet
  if(Channels == 2){
    /* Compute gains and delays for stereo imaging
       Extreme gain differences can make the source sound like it's inside an ear
       This can be uncomfortable in good headphones with extreme panning
       -6dB for each channel in the center
       when full to one side or the other, that channel is +6 dB and the other is -inf dB
    */
    double const left_gain = sp->gain * (1 - sp->pan)/2;
    double const right_gain = sp->gain * (1 + sp->pan)/2;
    /* Delay less favored channel 0 - 1.5 ms max (determined
       empirically) This is really what drives source localization
       in humans. The effect is so dramatic even with equal levels
       you have to remove one earphone to convince yourself that the
       levels really are the same!
    */
    int const left_delay = (sp->pan > 0) ? (int)round(sp->pan * .0015 * DAC_samprate) : 0; // Delay left channel
    int const right_delay = (sp->pan < 0) ? (int)round(-sp->pan * .0015 * DAC_samprate) : 0; // Delay right channel

    assert(left_delay >= 0 && right_delay >= 0);

    // Write into per-session buffer to be read by portaudio callback
    int64_t left_index = Channels * left_delay;
    int64_t right_index = Channels * right_delay + 1;

    if(sp->channels == 1){
      for(int i=0; i < sp->output_rate_data_size; i++){
	// Mono input, put on both channels
	double s = sp->output_rate_data[i];
	sp->buffer[BINDEX(base,left_index)] = (float)(s * left_gain);
	sp->buffer[BINDEX(base,right_index)] = (float)(s * right_gain);
	left_index += Channels;
	right_index += Channels;
      }
    } else {
      for(int i=0; i < sp->output_rate_data_size; i++){
	// stereo input
	double left = sp->output_rate_data[2*i];
	double right = sp->output_rate_data[2*i+1];
	sp->buffer[BINDEX(base,left_index)] = left * left_gain;
	sp->buffer[BINDEX(base,right_index)] = right * right_gain;
	left_index += Channels;
	right_index += Channels;
      }
    }
  } else { // Channels == 1, no panning
    if(sp->channels == 1){
      for(int i=0; i < sp->output_rate_data_size; i++){
	double s = sp->gain * sp->output_rate_data[i];
	sp->buffer[BINDEX(base,i)] = (float)s;
      }
    } else {  // sp->channels == 2
      for(int i=0; i < sp->output_rate_data_size; i++){
	// Downmix to mono
	double s = 0.5 * sp->gain * (sp->output_rate_data[2*i] + sp->output_rate_data[2*i+1]);
	sp->buffer[BINDEX(base,i)] = (float)s;
      }
    }
  } // if(sp->channels == 1)
  // Advance the official write pointer in units of frames
  wptr += sp->output_rate_data_size;
  // Write back atomically
  atomic_store_explicit(&sp->wptr,wptr,memory_order_release);
}
