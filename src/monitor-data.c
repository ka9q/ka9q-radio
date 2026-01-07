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
static void init_pl(struct session *sp);
static int run_pl(struct session *sp);
static void apply_notch(struct session *sp);
static int conceal(struct session *sp,int gap);
static int decode_rtp_data(struct session *sp,struct packet const *pkt);
static void copy_to_stream(struct session *sp);
static int upsample(struct session *sp);
static uint64_t reset_playout(struct session *sp);
static void *decode_task(void *arg);
static int calculate_deadline(struct timespec *deadline,int64_t timeout);
static int calculate_tight_deadline(struct timespec *deadline,struct session *sp);

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
      sp->dest = mcast_address_text;
      sp->initialized = true;
      if(pthread_create(&sp->task,NULL,decode_task,sp) != 0){
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
    const int maxq = 100; // 2 seconds
    pthread_mutex_lock(&sp->qmutex);
    for(qe = sp->queue;
	qe != NULL && qlen < maxq && pkt->rtp.seq >= qe->rtp.seq;
	q_prev = qe,qe = qe->next,qlen++)
      ;

    if(qlen >= maxq){
      // Queue has gotten out of control, blow it away. Seems to happen when a macos laptop is asleep
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

    // wake up decoder thread, a packet has appeared
    pthread_cond_broadcast(&sp->qcond);
    pthread_mutex_unlock(&sp->qmutex);
  }
  return NULL;
}
static void decode_task_cleanup(void *arg){
  struct session *sp = (struct session *)arg;
  assert(sp);

  atomic_store_explicit(&sp->inuse,false,memory_order_release);
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
static void *decode_task(void *arg){
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


  struct packet *pkt = NULL; // make sure it starts this way

  // status reception doesn't write below this point
  sp->pan = 0;     // center by default
  if(Auto_position)
    sp->pan = make_position(Position++);

  sp->gain = dB2voltage(Gain);    // Start with global default
  sp->notch_enable = Notch;
  sp->playout = (int)round(Playout * (double)DAC_samprate); // per-session playout is in frames
  atomic_store_explicit(&sp->muted,Start_muted,memory_order_release);
  sp->restart = true; // Force rest of init when first packet arrives
  struct timespec deadline = {0};
  // Main outer loop; run until inner loop senses a terminate
  while(true){
    if(gps_time_ns() > sp->last_active + BILLION/2){
      sp->active = 0; // no data for 500 ms. could probably also close squelch
      sp->squelch_open = false;
    }
    if(!sp->squelch_open || sp->samprate == 0 || sp->channels == 0){
      sp->plc_enable = false;
    } else {
      enum encoding encoding = sp->pt_table[sp->type].encoding;
      sp->plc_enable = (encoding == OPUS || encoding == OPUS_VOIP);
    }
    if(sp->plc_enable){
      // If we can send PLCs, sleep until the last millisecond to do them
      // compute max wait time for a new packet or a squelch state change
      calculate_tight_deadline(&deadline,sp);
    } else
      calculate_deadline(&deadline,10*BILLION);

    assert(pkt == NULL); // watch for leaks

    while(!terminated(sp)){ // inner loop until we get a packet to process
      // Wait until the deadline for an RTP packet
      assert(pkt == NULL); // should be cleared, otherwise it's a memory leak
      pthread_mutex_lock(&sp->qmutex);
      int rc = 0;
      do {
	pkt = sp->queue;
	if(pkt != NULL && (sp->restart || (int16_t)(pkt->rtp.seq - sp->next_seq)<=0))
	  break;	  // No reason to wait

	// The qcond condition is signaled when a new RTP frame appears
	Waits++;
	rc = pthread_cond_timedwait(&sp->qcond,&sp->qmutex,&deadline);
	rc &= 0xff;
	assert(rc == 0 || rc == ETIMEDOUT); // shouldn't fail for any other reason
      } while(rc != ETIMEDOUT);
      if(rc == ETIMEDOUT)
	Wait_timeout++;
      else
	Wait_successful++;

      if(pkt != NULL)
	sp->queue = pkt->next; // before we release the lock
      pthread_mutex_unlock(&sp->qmutex); // no longer examining queue

      if(rc == 0){
	// poison the deadline to try to force an error if I reuse it by mistake
	deadline.tv_sec = -1;
	deadline.tv_nsec = 2 * BILLION;
      }
      if(pkt != NULL){
	if(sp->restart){
	  sp->restart = false;
	  sp->next_seq = pkt->rtp.seq;
	  sp->next_timestamp = pkt->rtp.timestamp;
	  pkt->next = NULL;
	  reset_playout(sp);
	}
	if((int16_t)(pkt->rtp.seq - sp->next_seq) > 10){
	  // Allow a forward sequence jump of up to 10 dropped packets
	  // Accept old sequence numbers to get them out of the way, they're probably too old anyway
	  // But if it persists, consider a possible stream restart
	  if(++sp->consec_out_of_order < 6){
	    // Toss but count
	    sp->drops++;
	    FREE(pkt);
	    continue; // repeat inner loop with the SAME deadline
	  }
	  reset_playout(sp);
	  sp->next_timestamp = pkt->rtp.timestamp;
	} else if(pkt->rtp.marker){
	  reset_playout(sp);
	  sp->next_timestamp = pkt->rtp.timestamp;
	}
	sp->next_seq = pkt->rtp.seq + 1; // Expect the one after this next
	sp->last_timestamp = pkt->rtp.timestamp; // remember the latest for delay calcs
	sp->consec_erasures = 0;
	sp->consec_out_of_order = 0;
	decode_rtp_data(sp,pkt); // will pick up sp->samprate the first time

	// decoded data is in sp->bounce. where do we write it?
	// remember the sender's timestamp and our read pointer both increase steadily with real time
	int32_t jump = (int32_t)(pkt->rtp.timestamp - sp->next_timestamp);

	// convert to frames at DAC rate
	if(jump != 0){
	  if(sp->samprate == 0 || sp->channels == 0){
	    // Can't proceed, probably first use of a new payload type
	    FREE(pkt);
	    reset_playout(sp); // but don't fall behind for when it clears up
	    continue;
	  }
	  int write_adjust = (int64_t)jump * DAC_samprate / sp->samprate;
	  if(write_adjust != 0){
	    atomic_fetch_add_explicit(&sp->wptr,write_adjust,memory_order_release);	    // Adjust write pointer
	    uint64_t wptr = atomic_load_explicit(&sp->wptr,memory_order_relaxed);
	    if(wptr > sp->wptr_highwater)
	      sp->wptr_highwater = wptr;
	  }
	}
	sp->next_timestamp = pkt->rtp.timestamp + sp->frame_size;
	FREE(pkt);
	break; // data in sp->bounce, length in sp->frame_size, process in outer loop, recalculate timeout on next pass
      }
      // else timeout. pkt == NULL; nothing usable on queue
      if(sp->squelch_open && sp->plc_enable){
	// Look at our send queue to decide whether to issue a PLC
	// wptr and rptr are in stereo frames at DAC_samprate, typically 48 kHz
	int64_t q = qlen(sp);
	// Read and write pointers and sample rate  must be initialized to be useful
	// Look for less than 10 ms of margin. Probably actually a multiple of 20 ms
	if(q <= DAC_samprate/100 && sp->last_framesize != 0 && sp->consec_erasures++ < 6){
	  int n;
	  if((n = conceal(sp,sp->last_framesize)) > 0){
	    // Attempted packet conceal succeeded
	    sp->frame_size = n;
	    sp->next_seq++; // assume we've lost one, expect the next. If the lost one arrives late it will be dropped
	    sp->next_timestamp += sp->frame_size; // sp->framesize is returned by conceal()
	    break; // process the synthetic PLC samples in sp->bounce, recalulate timeout
	  }
	}
      }	// we've run dry but can't send a PLC
      sp->frame_size = 0;
      break; // no progress, let outer loop recalculate our deadline
    }// end of inner loop
    if(terminated(sp))
      goto done;
    // We have a frame of decoded audio or PLC from Opus
    // Do PL detection and notching even when muted
    if(sp->frame_size > 0 && sp->samprate != 0){
      // Limit to 1.5s on queue
      double q = qlen(sp);
      if(q < 0 || q > 1.5 * DAC_samprate)
	reset_playout(sp);

      if(sp->notch_enable){
	run_pl(sp);
	apply_notch(sp);
      }
      // count active time even when muted
      sp->tot_active += (double)sp->frame_size / sp->samprate;
      sp->active += (double)sp->frame_size / sp->samprate;

      upsample(sp);
      copy_to_stream(sp);
    }
  } // end of outer loop
 done:;
  pthread_cleanup_pop(1);
  return NULL;
}

// Reset playout buffer
// also reset Opus decoder, if present
static uint64_t reset_playout(struct session * const sp){
  sp->resets++;
  if(sp->opus)
    opus_decoder_ctl(sp->opus,OPUS_RESET_STATE); // Reset decoder

  uint64_t const rptr = atomic_load_explicit(&Output_time,memory_order_acquire);
  uint64_t wptr = rptr + sp->playout;
  atomic_store_explicit(&sp->wptr,wptr,memory_order_release);
  if(wptr > sp->wptr_highwater)
    sp->wptr_highwater = wptr;
  return wptr;
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
      if(!inuse(&Sessions[i]))
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
  int prev_channels = sp->channels;
  if(sp->pt_table[sp->type].encoding == OPUS){
    // The table values reflect the encoder input; they're for the status display
    // The encoder output is always forced to the local DAC
    sp->samprate = DAC_samprate;
    sp->channels = Channels;
  } else {
    // Use actual table values for PCM
    int samprate = sp->pt_table[sp->type].samprate;
    int channels = sp->pt_table[sp->type].channels;
    if(samprate == 0 || channels == 0){
      // Don't know the sample rate yet, we can't proceed
      // Probably because the pt_table wasn't populated yet by the status message using it for the first time
      sp->frame_size = 0;
      sp->drops++;
      return 0;
    }
    sp->samprate = samprate;
    sp->channels = channels;
  }
  if(sp->samprate != prev_samprate || sp->channels != prev_channels){
    reset_playout(sp);
    init_pl(sp);
  }

  if(pkt->len <= 0){
    sp->frame_size = 0;
    sp->empties++;
    sp->drops++;
    return 0;
  }
  if(sp->bounce == NULL)
    sp->bounce = malloc(2 * BBSIZE * sizeof *sp->bounce);
  assert(sp->bounce != NULL);

  // This section processes the signal in the current RTP frame, copying and/or decoding it into a sp->bounce buffer
  // for mixing with the output ring buffer
  enum encoding encoding = sp->pt_table[sp->type].encoding;
  int sampsize = 0;
  switch(encoding){
  case OPUS:
  case OPUS_VOIP:
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
      sp->drops++;
      return -2;
    }
    assert(r1 >= 0);
    sp->frame_size = r1;
    sp->last_framesize = r1;
    opus_int32 const r2 = opus_packet_get_bandwidth(pkt->data);
    sp->bandwidth = opus_bandwidth(NULL,r2);
    sp->opus_channels = opus_packet_get_nb_channels(pkt->data); // Only for display purposes. We always decode to output preference

    // by 'samples' they apparently mean stereo samples
    // Should probably blank any data with timestamp < expected timestamp since the decoder state won't be right
    if(pkt->rtp.timestamp == sp->next_timestamp){
      opus_int32 const decoded_samples = opus_decode_float(sp->opus,pkt->data,(opus_int32)pkt->len,
							   sp->bounce,
							   (int)sp->frame_size,0);
      assert(decoded_samples == (opus_int32)sp->frame_size); // Or something is broken inside Opus
      // Maintain smoothed measurement of data rate
      // Won't work right with discontinuous transmission - fix by looking at timestamps
      double const rate = 8 * pkt->len * DAC_samprate / (double)decoded_samples; // 8 bits/byte * length / (samples/samprate)
      sp->datarate += 0.1 * (rate - sp->datarate);
    } else
      memset(sp->bounce,0,sp->frame_size * sp->channels * sizeof *sp->bounce); // blank out of sequence
    break;
  case S16BE:
    {
      int16_t const * const data = (int16_t *)&pkt->data[0];
      sampsize = sizeof *data;
      sp->frame_size = pkt->len / (sampsize * sp->channels); // mono/stereo samples in frame
      for(int i=0; i < sp->channels * sp->frame_size; i++)
	sp->bounce[i] = SCALE16 * (int16_t)ntohs(data[i]); // Cast is necessary
      sp->datarate = 8 * sp->channels * sampsize * sp->samprate; // fixed rate
      sp->bandwidth = sp->samprate / 2; // Nyquist
    }
    break;
  case S16LE: // same as S16BE but no byte swap - assumes little-endian machine
    {
      int16_t const * const data = (int16_t *)&pkt->data[0];
      sampsize = sizeof *data;
      sp->frame_size = pkt->len / (sampsize * sp->channels); // mono/stereo samples in frame
      for(int i=0; i < sp->channels * sp->frame_size; i++)
	sp->bounce[i] = SCALE16 * data[i];
      sp->datarate = 8 * sp->channels * sampsize * sp->samprate; // fixed rate
      sp->bandwidth = sp->samprate / 2; // Nyquist
    }
    break;
  case F32BE:
    {
      float const * const data = (float *)&pkt->data[0];
      sampsize = sizeof *data;
      sp->frame_size = pkt->len / (sampsize * sp->channels); // mono/stereo samples in frame
      for(int i=0; i < sp->channels * sp->frame_size; i++)
	sp->bounce[i] = data[i];
      sp->datarate = 8 * sp->channels * sampsize * sp->samprate; // fixed rate
      sp->bandwidth = sp->samprate / 2; // Nyquist
    }
    break;
#ifdef HAS_FLOAT16
  case F16BE: // 16-bit floats
    {
      float16_t const * const data = (float16_t *)&pkt->data[0];
      sampsize = sizeof *data;
      sp->frame_size = pkt->len / (sampsize * sp->channels); // mono/stereo samples in frame
      for(int i=0; i < sp->channels * sp->frame_size; i++)
	sp->bounce[i] = data[i];
      sp->datarate = 8 * sp->channels * sampsize * sp->samprate; // fixed rate
      sp->bandwidth = sp->samprate / 2; // Nyquist
    }
    break;
#endif
  default:
    sp->drops++;
    sp->frame_size = 0;
    sampsize = 0;
    return -2; // No change to next_timestamp or wptr because we don't know the sample size
  } // end of encoding switch
  sp->last_framesize = sp->frame_size;
  return 0;
}

// Called when there isn't an in-sequence packet to be processed
// Takes length to be concealed at DAC samprate, must br legal opus
// returns length of generated plc
static int conceal(struct session *sp,int gap){
  assert(sp != NULL);
  if(sp == NULL)
    return 0;

  assert(legal_opus_size(gap));
  enum encoding encoding = sp->pt_table[sp->type].encoding;

  if((encoding != OPUS && encoding != OPUS_VOIP) || !legal_opus_size(gap))
    return 0;

  // Trigger loss concealment, up to 6 consecutive packets (max Opus packet is 120 ms)
  sp->plcs++;
  opus_int32 const frame_count = opus_decode_float(sp->opus,NULL,0,sp->bounce,gap,0);
  (void)frame_count;
  assert(frame_count == gap);
  return frame_count; // how much we moved, even if not opus
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
  if(sp->channels == 1){
    for(int i=0; i < sp->frame_size; i++){
      double s = sp->bounce[i];
      for(int j = 0; j < N_tones; j++)
	update_goertzel(&sp->tone_detector[j],s);
    }
  } else {
    for(int i=0,k=0; i < sp->frame_size; i++,k+= 2){
      double s = 0.5 * (sp->bounce[k] + sp->bounce[k+1]);
      for(int j = 0; j < N_tones; j++)
	update_goertzel(&sp->tone_detector[j],s);
    }
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

  if(sp->notch_tone <= 0)
    return;

  // Do this even when not selected by voting, to prevent transients when it's selected
  if(sp->channels == 1){
    for(int i = 0; i < sp->frame_size; i++)
      sp->bounce[i] = (float)applyIIR(&sp->iir_left,sp->bounce[i]);
  } else {
    for(int i = 0,k=0; i < sp->frame_size; i++,k += 2){
      sp->bounce[k] = (float)applyIIR(&sp->iir_left,sp->bounce[k]);
      sp->bounce[k+1] = (float)applyIIR(&sp->iir_right,sp->bounce[k+1]);
    }
  }
}
// Upsample to DAC rate if necessary
static int upsample(struct session * const sp){
  assert(sp != NULL && sp->samprate != 0 && sp->channels != 0);
  if(sp == NULL || sp->samprate == 0 || sp->channels == 0)
    return -1;

  if(sp->frame_size == 0)
    return 0;

  if(sp->samprate == DAC_samprate)
    return sp->frame_size;   // No conversion necessary

  double upsample_ratio = (double)DAC_samprate / sp->samprate;
  assert(sp->frame_size * upsample_ratio <= BBSIZE);
  if(sp->frame_size * upsample_ratio  > BBSIZE)
    return -1;

  int error = 0;
  if(sp->channels == 1 && sp->src_state_mono == NULL){
    sp->src_state_mono = src_new(SRC_SINC_FASTEST, sp->channels, &error);
    assert(sp->src_state_mono != NULL);
  } else if(sp->channels == 2 && sp->src_state_stereo == NULL){
    sp->src_state_stereo = src_new(SRC_SINC_FASTEST, sp->channels, &error);
    assert(sp->src_state_stereo != NULL);
  }
  SRC_DATA src_data = {
    .data_in = sp->bounce,  // Pointer to input audio
    .input_frames = sp->frame_size,
    .output_frames = BBSIZE,
    .data_out = malloc(2 * BBSIZE * sizeof(float)), // assume stereo for safety
    .src_ratio = upsample_ratio,
    .end_of_input = 0
  };

  src_set_ratio (sp->channels == 1 ? sp->src_state_mono : sp->src_state_stereo, upsample_ratio);

  error = src_process(sp->channels == 1 ? sp->src_state_mono : sp->src_state_stereo, &src_data);
  if(error != 0)
    fprintf(stderr,"src_process: %s\n",src_strerror(error));
  assert(error == 0);
  FREE(sp->bounce);
  // Replace input pointer with output
  sp->bounce = src_data.data_out;
  sp->frame_size = src_data.output_frames_gen;
  if(src_data.input_frames != src_data.input_frames_used){
    fprintf(stderr,"ssrc %d src ratio %lf: in given %ld, taken %ld, out asked %ld, out given %ld\n",
	    sp->ssrc,src_data.src_ratio,src_data.input_frames, src_data.input_frames_used,src_data.output_frames,src_data.output_frames_gen);
  }
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

  {
    // Measure output audio level
    double energy = 0;
    for(int i=0; i < sp->frame_size * sp->channels; i++)
      energy += sp->bounce[i] * sp->bounce[i];

    energy /= sp->frame_size * sp->channels;
    if(isfinite(energy))
      sp->level += .01 * (energy - sp->level); // smooth
  }

  double q = qlen(sp);

  // Use these for playout buffer adjustments
  if(q < 0){
    sp->lates++;
    sp->consec_lates++;
    sp->consec_earlies = 0;
    //  } else if((queue_in_frames + sp->frame_size) * Channels > BUFFERSIZE){ // entire buffer
  } else if(q > 2 * sp->playout){
    sp->earlies++;
    sp->consec_earlies++;
    sp->consec_lates = 0;
  } else {
    sp->consec_earlies = 0;
    sp->consec_lates = 0;
  }
  if(sp->consec_lates > 6 || sp->consec_earlies > 6){
    reset_playout(sp);
    sp->consec_lates = sp->consec_earlies = 0;
  }
  uint64_t wptr = atomic_load_explicit(&sp->wptr,memory_order_relaxed); // frames

  if(muted(sp))
    goto advance; // skip the copy

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
      for(int i=0; i < sp->frame_size; i++){
	// Mono input, put on both channels
	double s = sp->bounce[i];
	sp->buffer[BINDEX(base,left_index)] = (float)(s * left_gain);
	sp->buffer[BINDEX(base,right_index)] = (float)(s * right_gain);
	left_index += Channels;
	right_index += Channels;
      }
    } else {
      for(int i=0; i < sp->frame_size; i++){
	// stereo input
	double left = sp->bounce[2*i];
	double right = sp->bounce[2*i+1];
	sp->buffer[BINDEX(base,left_index)] = left * left_gain;
	sp->buffer[BINDEX(base,right_index)] = right * right_gain;
	left_index += Channels;
	right_index += Channels;
      }
    }
  } else { // Channels == 1, no panning
    if(sp->channels == 1){
      for(int i=0; i < sp->frame_size; i++){
	double s = sp->gain * sp->bounce[i];
	sp->buffer[BINDEX(base,i)] = (float)s;
      }
    } else {  // sp->channels == 2
      for(int i=0; i < sp->frame_size; i++){
	// Downmix to mono
	double s = 0.5 * sp->gain * (sp->bounce[2*i] + sp->bounce[2*i+1]);
	sp->buffer[BINDEX(base,i)] = (float)s;
      }
    }
  } // if(sp->channels == 1)
 advance:;
  // Always advance the official write pointer in units of output frames
  // even when muted
  wptr += sp->frame_size;
  // Write back atomically
  atomic_store_explicit(&sp->wptr,wptr,memory_order_release);
  if(wptr > sp->wptr_highwater)
    sp->wptr_highwater = wptr;
}
static int calculate_deadline(struct timespec *deadline,int64_t timeout){
  assert(deadline != NULL);
  int r = clock_gettime(CLOCK_REALTIME,deadline);
  assert(r == 0);
  (void)r;

  if(timeout > 0){
    deadline->tv_nsec += timeout % BILLION;
    deadline->tv_sec += timeout / BILLION;
    if(deadline->tv_nsec >= BILLION){
      deadline->tv_nsec -= BILLION;
      deadline->tv_sec++;
    }
  }
  return 0;
}
// calculate deadline for when active queue will dry up
static int calculate_tight_deadline(struct timespec *deadline,struct session *sp){
  int64_t frames = qlen(sp);
  if(frames <= 0)
    return calculate_deadline(deadline,0); // from now

  return calculate_deadline(deadline,BILLION * frames / DAC_samprate);
}
// Frames @ DAC rate still to be played out
int64_t qlen(struct session const *sp){
  uint64_t const rptr = atomic_load_explicit(&Output_time,memory_order_acquire); // the callback writes it
  uint64_t const wptr = atomic_load_explicit(&sp->wptr,memory_order_relaxed); // only we write it
  if(rptr == 0 || wptr == 0)
    return 0; // probably not initialized yet
  int64_t qlen = (int64_t)(wptr - rptr)
    - (int64_t)ceil(DAC_samprate * (Pa_GetStreamTime(Pa_Stream) - atomic_load_explicit(&Last_callback_time,memory_order_acquire)));

  return qlen;
}
