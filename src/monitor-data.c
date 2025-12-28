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
static void init_pl(struct session *sp);
static int run_pl(struct session *sp);
static void apply_notch(struct session *sp);
static int conceal(struct session *sp,uint32_t timestamp);
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
  while(!atomic_load_explicit(&Terminate,memory_order_acquire)){
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
    if(!sp->initialized){
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

      if(pthread_create(&sp->task,NULL,decode_task,sp) != 0){
	perror("pthread_create");
	close_session(sp);
	continue;
      }
      sp->initialized = true;
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

  sp->inuse = false;
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
  FREE(sp->buffer);
  FREE(sp->bounce);

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
  if(sp->buffer == NULL)
    sp->buffer = malloc(BUFFERSIZE * sizeof *sp->buffer);

  if(sp->bounce == NULL)
    sp->bounce = malloc(BBSIZE * sizeof *sp->bounce);

  struct packet *pkt = NULL; // make sure it starts this way
  reset_playout(sp);
  bool init = false;
  // Main loop; run until asked to quit
  while(!atomic_load_explicit(&sp->terminate,memory_order_acquire) && !atomic_load_explicit(&Terminate,memory_order_acquire)){
    if(sp->samprate == 0){
      sp->running = false;
      sp->active = 0;
    }
    while(!atomic_load_explicit(&sp->terminate,memory_order_acquire) && !atomic_load_explicit(&Terminate,memory_order_acquire)){
      // Try to receive and process a RTP packet. Break when we have.
      assert(pkt == NULL); // should be cleared, otherwise it's a memory leak
      // The queue is sorted by sequence number. Anything on it?
      pthread_mutex_lock(&sp->qmutex);
      pkt = sp->queue;
      uint32_t rtp_timestamp = 0;
      if(pkt != NULL){
	// We have the lock while we figure out if we want it
	if(!init){
	  // Very first packet, force acceptance
	  sp->rtp_state.seq = pkt->rtp.seq; // expect the first sequence number
	  sp->next_timestamp = pkt->rtp.timestamp; // and timestamp
	  init = true;
	}
	// Peek at the queue and find the time jump to the next waiting packet, if any
	// If we see an out of order packet at the head of the queue, take it after 6 loops
	if(pkt->rtp.seq == sp->rtp_state.seq || ++sp->consec_out_of_order >= 6){
	  sp->queue = sp->queue->next;
	  pthread_mutex_unlock(&sp->qmutex); // not looking at the queue anymore
	  pkt->next = NULL;
	  sp->rtp_state.seq = pkt->rtp.seq + 1; // Expect the one after this next
	  sp->consec_erasures = 0;
	  if(sp->consec_out_of_order > 0){
	    // resynch ("Vat are you resynching about?")
	    sp->next_timestamp = pkt->rtp.timestamp;
	    sp->consec_out_of_order = 0;
	  }
	  if(!sp->running){
	    sp->running = true;
	    reset_playout(sp);
	  }
	  decode_rtp_data(sp,pkt);
	  FREE(pkt);
	  break; // we've got a weiner
	}
	// Out of sequence, grab its RTP timestamp, leave it on queue
	// We'll see it on each new poll, though
	rtp_timestamp = pkt->rtp.timestamp;
	pkt = NULL;
      } // fall through
      // nothing usable on queue - lock still held
      // How does our output queue look?
      // wptr and rptr are in frames at DAC_samprate, typically 48 kHz
      if(sp->reset)
	reset_playout(sp); // Use the most up-to-date wptr based on Output_time

      // Calculate queue wait timeout
      uint64_t timeout = (int64_t)BILLION/10; // default timeout 100 ms when stopped
      uint64_t const rptr = atomic_load_explicit(&Output_time,memory_order_relaxed);
      uint64_t const wptr = atomic_load_explicit(&sp->wptr,memory_order_relaxed);
      // Read and write pointers and sample rate  must be initialized to be useful
      if(rptr == 0 || wptr == 0){
	// Probably hasn't ever run - can this happen? maybe the callback isn't running yet
	sp->running = false;
	sp->active = 0;
      }
      if(sp->running && !sp->muted){
	// wptr and rptr are in frames at DAC_samprate, typically 48 kHz
	unsigned const quantum = atomic_load_explicit(&Callback_quantum,memory_order_relaxed);

	if(wptr > rptr + sp->playout + quantum){ // care with unsigned!
	  // We're conservatively ahead of the reader, by how much?
	  uint64_t margin = wptr - (rptr + sp->playout + quantum); // must be positive
	  timeout = BILLION * margin / DAC_samprate;	  // Playout queue is happy, we don't need to do anything right now
	  if(timeout > (uint64_t)BILLION/10)
	    timeout = BILLION/10; // Limit the timeout to 100 ms just in case rptr got hung up, eg, by suspend
	} else {
	  // Ran out of time, try to conceal the packet loss
	  // if rtp_timestamp != 0, it's from the RTP packet waiting resequencing
	  if(conceal(sp,rtp_timestamp) > 0) {
	    pthread_mutex_unlock(&sp->qmutex); // finally release it
	    break; // conceal generated, handle the synthetic audio
	  } else {
	    // Too many PLCs generated, stream stopped
	    sp->running = false;
	    sp->active = 0;
	  }
	}
      }
      // Sleep unil a packet arrives or our deadline
      // we're still holding the lock
      {
	struct timespec deadline = {0};
	if(clock_gettime(CLOCK_REALTIME,&deadline) != 0){
	  fprintf(stderr,"clock_gettime(CLOCK_REALTIME): %s\n",strerror(errno));
	  assert(false);
	  break;
	}
	deadline.tv_nsec += timeout % BILLION;
	deadline.tv_sec += timeout / BILLION;
	pthread_cond_timedwait(&sp->qcond,&sp->qmutex,&deadline);
      }
      pthread_mutex_unlock(&sp->qmutex); // finally release it
    }

    if(atomic_load_explicit(&sp->terminate,memory_order_acquire) || atomic_load_explicit(&Terminate,memory_order_acquire))
      break;

    // We get here with a frame of decoded audio, possibly PLC from Opus
    // Do PL detection and notching even when muted or outvoted
    if(sp->samprate == 0 || sp->channels == 0||sp->frame_size == 0)
      continue; // Not yet fully initialized

    if(sp->notch_enable){
      run_pl(sp);
      apply_notch(sp);
    }
    // count active time even when muted
    sp->tot_active += (double)sp->frame_size / sp->samprate;
    sp->active += (double)sp->frame_size / sp->samprate;

    if(!sp->muted && (!Voting || Best_session == sp)){
      upsample(sp);
      copy_to_stream(sp);
    }
  }
  pthread_cleanup_pop(1);
  return NULL;
}

// Reset playout buffer
// also reset Opus decoder, if present
void reset_playout(struct session * const sp){
  sp->resets++;
  if(sp->opus)
    opus_decoder_ctl(sp->opus,OPUS_RESET_STATE); // Reset decoder
  sp->reset = false;
  sp->playout = (int)round(Playout * DAC_samprate);

  uint64_t const rptr = atomic_load_explicit(&Output_time,memory_order_acquire);
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
      if(!Sessions[i].inuse)
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
  if(sp->samprate == 0){
    // Don't know the sample rate yet, we can't proceed
    sp->rtp_state.drops++; // may cause big burst at start of stream
    return 0;
  }
  if(sp->samprate != prev_samprate)
    init_pl(sp);

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
  enum encoding encoding = sp->pt_table[sp->type].encoding;
  if(encoding == OPUS || encoding == OPUS_VOIP){ // really the same codec, signalled in band
    // The Opus decoder output is always forced to the local channel count and sample rate
    // The values in the table reflect the *encoder input*
    if(!sp->opus){
      // This should happen only once on a stream
      // Always decode Opus to local DAC rate of 48 kHz and channel count
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
  int sampsize = 0;
  switch(encoding){
  case S16BE:
    {
      int16_t const * const data = (int16_t *)&pkt->data[0];
      sampsize = sizeof *data;
      sp->frame_size = pkt->len / (sampsize * sp->channels); // mono/stereo samples in frame
      for(int i=0; i < sp->channels * sp->frame_size; i++)
	sp->bounce[i] = SCALE16 * (int16_t)ntohs(data[i]); // Cast is necessary
    }
    break;
  case S16LE: // same as S16BE but no byte swap - assumes little-endian machine
    {
      int16_t const * const data = (int16_t *)&pkt->data[0];
      sampsize = sizeof *data;
      sp->frame_size = pkt->len / (sampsize * sp->channels); // mono/stereo samples in frame
      for(int i=0; i < sp->channels * sp->frame_size; i++)
	sp->bounce[i] = SCALE16 * data[i];
    }
    break;
  case F32LE:
    {
      float const * const data = (float *)&pkt->data[0];
      sampsize = sizeof *data;
      sp->frame_size = pkt->len / (sampsize * sp->channels); // mono/stereo samples in frame
      for(int i=0; i < sp->channels * sp->frame_size; i++)
	sp->bounce[i] = data[i];
    }
    break;
#ifdef HAS_FLOAT16
  case F16LE: // 16-bit floats
    {
      float16_t const * const data = (float16_t *)&pkt->data[0];
      sampsize = sizeof *data;
      sp->frame_size = pkt->len / (sampsize * sp->channels); // mono/stereo samples in frame
      for(int i=0; i < sp->channels * sp->frame_size; i++)
	sp->bounce[i] = data[i];
    }
    break;
#endif
  default:
    sp->rtp_state.drops++;
    sp->frame_size = 0;
    sampsize = 0;
    return -2; // No change to next_timestamp or wptr because we don't know the sample size
  } // end of PCM switch
  sp->bandwidth = sp->samprate / 2; // Nyquist
  sp->datarate = 8 * sp->channels * sampsize * sp->samprate; // fixed rate
  return 0;
}

// Called when there isn't an in-sequence packet to be processed
// Return 0 when the output stream is already in good shape
// Return -1 if stream is not initialized and running
// Return >0 when length of generated PLC frame
static int conceal(struct session *sp,uint32_t timestamp){
  assert(sp != NULL);
  if(sp == NULL)
    return -1;

  // Time has run out, so we gotta do something; generate some PLC or silence
  int time_diff = timestamp != 0 ? (int32_t)(timestamp - sp->next_timestamp) : 0;

  // if timestamp != 0 there's a gap, use it to calculate gap
  // This is not necessarily a lost packet, the stream might have stopped
  int plc_size = (int)round(sp->samprate * .02); // fake a frame of this length
  // time_diff is nonzero only if there's a gap before the next packet on the queue
  if(time_diff > 0 && plc_size > time_diff)
    plc_size = time_diff; // But limit to the gap, if we know what it is

  if(++sp->consec_erasures <= 6 && sp->opus && legal_opus_size(plc_size)){
    // Trigger loss concealment, up to 6 consecutive packets (max Opus packet is 120 ms)
    sp->plcs++;
    opus_int32 const frame_count = opus_decode_float(sp->opus,NULL,0,
					       sp->bounce,plc_size,0);
    (void)frame_count;
    assert(frame_count == plc_size);
    sp->frame_size = plc_size;
    return plc_size;
  }
  return -1;
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

  if(sp->samprate == DAC_samprate)
    return sp->frame_size;   // No conversion necessary

  double upsample_ratio = DAC_samprate / sp->samprate;
  assert(sp->frame_size * upsample_ratio * sp->channels <= BBSIZE);
  if(sp->frame_size * upsample_ratio * sp->channels > BBSIZE)
    return -1;

  int error;
  if(sp->channels == 1 && sp->src_state_mono == NULL){
    sp->src_state_mono = src_new(SRC_SINC_FASTEST, sp->channels, &error);
    assert(sp->src_state_mono != NULL);
  } else if(sp->channels == 2 && sp->src_state_stereo == NULL){
    sp->src_state_stereo = src_new(SRC_SINC_FASTEST, sp->channels, &error);
    assert(sp->src_state_stereo != NULL);
  }
  SRC_DATA src_data = {0};
  src_data.data_in = sp->bounce;  // Pointer to input audio
  src_data.input_frames = sp->frame_size;
  src_data.output_frames = BBSIZE;
  src_data.data_out = malloc(BBSIZE * sizeof(float));

  src_data.src_ratio = upsample_ratio;
  src_data.end_of_input = 0;
  if(sp->channels == 1)
    error = src_process(sp->src_state_mono, &src_data);
  else
    error = src_process(sp->src_state_stereo, &src_data);
  if(error != 0)
    fprintf(stderr,"src_process: %s\n",src_strerror(error));
  assert(error == 0);
  FREE(sp->bounce);
  // Replace input pointer with output
  sp->bounce = src_data.data_out;
  sp->frame_size = (int)round(sp->frame_size * upsample_ratio);
  return sp->frame_size;
}

// Copy from bounce buffer to streaming output buffer read by Portaudio callback
// ASSUMES sp->bounce has DAC_samprate
static void copy_to_stream(struct session *sp){
  assert(sp != NULL);
  if(sp == NULL)
    return;

  // Figure out where to write into output buffer
  // We didn't actully know how many samples we have to write until after decoding
  //
  if(sp->frame_size == 0)
    return;

  float *trimmed = sp->bounce;
  int tsize = sp->frame_size;

  uint64_t rptr = atomic_load_explicit(&Output_time,memory_order_relaxed); // frames
  uint64_t wptr = atomic_load_explicit(&sp->wptr,memory_order_relaxed); // frames

  if(wptr < rptr){
    // we're late; can we salvage anything?
    int64_t overage = rptr - wptr; // Okay because we know it's positive
    if(overage < tsize){
      // the tail can still be sent
      trimmed += overage;
      wptr += overage;
      tsize -= overage;
    } else {
      // All of it is too late
      sp->lates++;
      if(++sp->consec_lates < 6)
	return; // might be an outlier Don't update wptr
      reset_playout(sp);  // drastic measures are needed, reset wptr
    }
  }
  {
    // Measure output audio level
    int count = tsize;
    if(sp->channels == 2)
      count *= 2;

    double energy = 0;
    for(int i=0; i < count; i++)
      energy += trimmed[i] * trimmed[i];

    energy /= count;
    if(isfinite(energy))
      sp->level += .01 * (energy - sp->level); // smooth
  }
  // Mix output sample rate data into output ring buffer
  int base = (wptr * Channels) & (BUFFERSIZE-1); // Base of output buffer for this packet
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
      for(int i=0; i < tsize; i++){
	// Mono input, put on both channels
	double s = trimmed[i];
	sp->buffer[BINDEX(base,left_index)] = (float)(s * left_gain);
	sp->buffer[BINDEX(base,right_index)] = (float)(s * right_gain);
	left_index += Channels;
	right_index += Channels;
      }
    } else {
      for(int i=0; i < tsize; i++){
	// stereo input
	double left = trimmed[2*i];
	double right = trimmed[2*i+1];
	sp->buffer[BINDEX(base,left_index)] = left * left_gain;
	sp->buffer[BINDEX(base,right_index)] = right * right_gain;
	left_index += Channels;
	right_index += Channels;
      }
    }
  } else { // Channels == 1, no panning
    if(sp->channels == 1){
      for(int i=0; i < tsize; i++){
	double s = sp->gain * trimmed[i];
	sp->buffer[BINDEX(base,i)] = (float)s;
      }
    } else {  // sp->channels == 2
      for(int i=0; i < tsize; i++){
	// Downmix to mono
	double s = 0.5 * sp->gain * (trimmed[2*i] + trimmed[2*i+1]);
	sp->buffer[BINDEX(base,i)] = (float)s;
      }
    }
  } // if(sp->channels == 1)
  // Advance the official write pointer in units of frames
  wptr += tsize;
  // Write back atomically
  atomic_store_explicit(&sp->wptr,wptr,memory_order_release);
}
