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

#if 0
static int buffer_margin(struct session const *sp);
#endif



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
    struct sockaddr group = {0};;

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
      sp->gain = pow(10.,0.05 * Gain);    // Start with global default
      sp->notch_enable = Notch;
      sp->muted = Start_muted;
      sp->dest = mcast_address_text;
      sp->rtp_state.timestamp = sp->next_timestamp = pkt->rtp.timestamp;
      sp->rtp_state.seq = pkt->rtp.seq;
      sp->reset = true;
      sp->init = true;

      if(pthread_create(&sp->task,NULL,decode_task,sp) == -1){
	perror("pthread_create");
	close_session(&sp);
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
  // Just in case anything was allocated for these arrays
  struct channel * const chan = &sp->chan;
  FREE(chan->spectrum.bin_data);
  FREE(chan->status.command);
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

  int consec_lates = 0;
  int consec_out_of_order = 100;  // Force sequence resynch first time
  uint64_t consec_erasures = 100; // Inhibit a PLC right out of the gate
  int const maxsamples = 5760; // Opus 120ms stereo @ 48 kHz
  int bounce_size = 2 * maxsamples * sizeof(float);
  float * bounce = calloc(1,bounce_size);
  assert(bounce != NULL);
  float *rate_converted_buffer = NULL;
  int rate_converted_buffer_size = 0;
  SRC_STATE *src_state_mono = NULL;
  SRC_STATE *src_state_stereo = NULL;

  reset_playout(sp);
  bool init = false;

  // Main loop; run until asked to quit
  while(!sp->terminate && !Terminate){
    if(sp->reset) // Requested by somebody
      reset_playout(sp);
    // The queue is sorted by sequence number. Anything on it?
    int seq_diff = 0;
    int time_diff = 0;
    struct packet *pkt = NULL; // make sure it starts this way
    pthread_mutex_lock(&sp->qmutex);
    if((pkt = sp->queue) != NULL){
      if(!init){
	sp->rtp_state.seq = pkt->rtp.seq; // expect the first sequence number
	sp->next_timestamp = pkt->rtp.timestamp; // and timestamp
	init = true;
      } else {
	// The casts ensure the sign bit is propagated from the unsigned short subtraction
	seq_diff = (int16_t)(pkt->rtp.seq - sp->rtp_state.seq);
      }
      if(seq_diff != 0){
	if(++consec_out_of_order < 6){
	  // old or future?
	  if(seq_diff < 0){
	    // Old duplicate, discard and look again
	    sp->queue = sp->queue->next;
	    pthread_mutex_unlock(&sp->qmutex);
	    FREE(pkt);
	    sp->rtp_state.drops++;
	    Last_error_time = gps_time_ns();
	    continue; // immediately look again
	  } else {
	    // seq_diff > 0: missing the next expected packet.
	    // leave the packet on the queue
	    // notify the code below that there is a pending gap
	    // let it decide if it wants to emit silence or PLC
	    time_diff = (int32_t)(pkt->rtp.timestamp - sp->next_timestamp);
	    pkt = NULL;
	  }
	} else {
	  // grab it, we're resynching ("What are you resynching about?")
	  sp->queue = sp->queue->next;
	  pkt->next = NULL;
	  reset_playout(sp);
	  sp->next_timestamp = pkt->rtp.timestamp;
	  sp->rtp_state.seq = pkt->rtp.seq + 1; // Expect the one after this next
	  time_diff = 0;
	  seq_diff = 0;
	  consec_out_of_order = 0;
	}
      } else { // seq_diff == 0
	// Normal, in sequence
	sp->queue = sp->queue->next;	
	pkt->next = NULL;
	sp->rtp_state.seq = pkt->rtp.seq + 1; // Expect the one after this next
	consec_out_of_order = 0;
	time_diff = (int32_t)(pkt->rtp.timestamp - sp->next_timestamp);
      }
    }
    pthread_mutex_unlock(&sp->qmutex);

    // We're here either with a regular frame to process, or nothing
    // Now we look at timestamps
    if(!pkt){
      // If seq_diff == 0, the queue is empty.
      // if seq_diff > 0, there's a gap and we've noted seq_diff and time_diff
      // This is not necessarily a lost packet, the stream might have stopped
      int rptr = atomic_load_explicit(&Rptr,memory_order_relaxed);
      if(sp->samprate == 0 || modsub(sp->wptr,rptr,BUFFERSIZE) > sp->playout){
	// Playout queue is happy, we don't need to do anything right now
	// OR we don't know the sample rate yet, so we can only wait
	usleep(5000); // 5 ms - do this right based on how much is in playout
	continue;
      }      
      // Time has run out, so we gotta do something; generate up to 20 ms of PLC or silence
      // time_diff was set above
      sp->frame_size = (int)round(sp->samprate * .02); // fake a frame of this length
      // time_diff is nonzero only if there's a gap before the next packet on the queue
      assert(seq_diff >= 0);
      if(seq_diff != 0 && time_diff > 0 && sp->frame_size > time_diff)
	sp->frame_size = time_diff; // But limit to the gap, if we know what it is

      ++consec_erasures; // very big counter
      if(sp->opus && consec_erasures <= 3 && legal_opus_size(sp->frame_size)){
	// Trigger loss concealment, up to 3 consecutive packets
	sp->plcs++;
	opus_int32 const count = opus_decode_float(sp->opus,NULL,0,bounce,sp->frame_size,0);
	(void)count;
	assert(count == sp->frame_size);
      } else {
	memset(bounce,0,bounce_size);	// PCM or Opus silence
      }
      // End of processing dummy packet to set up PLC or silence
      // This will advance both sp->wptr and sp->next_timestamp by sp->frame_size
      goto advance_out; // Only advancr wptr, not the next timestamp
    }
    // Process an in-sequence packet
    // It might be a little off due to PLC/silence we emitted
    if(pkt->rtp.marker // burst start
       || (sp->samprate != 0 && llabs((int64_t)time_diff * DAC_samprate / sp->samprate) > BUFFERSIZE/4)){
      // Somewhat arbitrary limit on time jumps
      reset_playout(sp);
      time_diff = 0;
    }
    consec_erasures = 0;
    sp->rtp_state.timestamp = pkt->rtp.timestamp; // used for delay calcs
    if(pkt->len <= 0){
      sp->frame_size = 0;
      sp->empties++;
      goto endloop; // No data to process
    }    
    sp->type = pkt->rtp.type;
    // This section processes the signal in the current RTP frame, copying and/or decoding it into a bounce buffer
    // for mixing with the output ring buffer
    double upsample_ratio;
    enum encoding const encoding = sp->pt_table[sp->type].encoding;
    if(encoding == OPUS){
      // The Opus decoder is always forced to the local channel count because the input stream can switch at any time
      // (e.g., I/Q vs envelope) without changing the payload type, so there could be a glitch
      // before the channel count in the status message catches up with us and we can initialize a new decoder
      sp->samprate = OPUS_SAMPRATE;
      upsample_ratio = (double)DAC_samprate / OPUS_SAMPRATE; // Opus always encodes to 48k. Usually the system is too.
      sp->channels = Channels;
      if(!sp->opus){
	// This should happen only once on a stream
	// Always decode Opus to DAC rate of 48 kHz, stereo
	int error;
	sp->opus = opus_decoder_create(OPUS_SAMPRATE,sp->channels,&error);
	if(error != OPUS_OK)
	  fprintf(stderr,"opus_decoder_create error %d\n",error);
	
	assert(sp->opus);
	opus_decoder_ctl(sp->opus,OPUS_SET_COMPLEXITY(10)); // Turn on all the new cool stuff
	
	// Init PL tone detectors
	for(int j=0; j < N_tones; j++)
	  init_goertzel(&sp->tone_detector[j],PL_tones[j]/(double)sp->samprate);
	sp->notch_tone = 0;
      }
      opus_int32 const r1 = opus_packet_get_nb_samples(pkt->data,(opus_int32)pkt->len,sp->samprate);
      if(r1 == OPUS_INVALID_PACKET || r1 == OPUS_BAD_ARG){
	sp->frame_size = 0;
	goto endloop; // Treat as lost? We don't know how to advance the timestamp
      }      
      assert(r1 >= 0);
      sp->frame_size = r1;
      opus_int32 const r2 = opus_packet_get_bandwidth(pkt->data);
      switch(r2){
      case OPUS_INVALID_PACKET:
      case OPUS_BAD_ARG:
	sp->bandwidth = 0;
	break;
      case OPUS_BANDWIDTH_NARROWBAND:
	sp->bandwidth = 4;
	break;
      case OPUS_BANDWIDTH_MEDIUMBAND:
	sp->bandwidth = 6;
	break;
      case OPUS_BANDWIDTH_WIDEBAND:
	sp->bandwidth = 8;
	break;
      case OPUS_BANDWIDTH_SUPERWIDEBAND:
	sp->bandwidth = 12;
	break;
      default:
      case OPUS_BANDWIDTH_FULLBAND:
	sp->bandwidth = 20;
	break;
      }
      sp->opus_channels = opus_packet_get_nb_channels(pkt->data); // Only for display purposes. We always decode to output preference
      assert(bounce != NULL);
      opus_int32 const decoded_samples = opus_decode_float(sp->opus,pkt->data,(opus_int32)pkt->len,bounce,(int)sp->frame_size,0);
      assert(decoded_samples == (opus_int32)sp->frame_size); // Or something is broken inside Opus
      // Maintain smoothed measurement of data rate
      // Won't work right with discontinuous transmission - fix by looking at timestamps
      double const rate = 8 * pkt->len * OPUS_SAMPRATE / (double)decoded_samples; // 8 bits/byte * length / (samples/samprate)
      sp->datarate += 0.1 * (rate - sp->datarate);
    } else { // PCM
      sp->channels = sp->pt_table[sp->type].channels;
      int const samprate = sp->pt_table[sp->type].samprate;
      if(samprate != sp->samprate){
	sp->samprate = samprate;
	// Reinit tone detectors whenever sample rate changes
	for(int j=0; j < N_tones; j++)
	  init_goertzel(&sp->tone_detector[j],PL_tones[j]/(double)sp->samprate);
	sp->current_tone = sp->notch_tone = 0; // force it to be re-detected at new sample rate
	sp->bandwidth = samprate / 2000;    // in kHz allowing for Nyquist, using actual input sample rate for Opus
      }
      upsample_ratio = (double)DAC_samprate / sp->samprate;
	
      // decode PCM into bounce buffer
      switch(encoding){
      case S16LE:
      case S16BE:
	sp->datarate = 8 * sp->channels * sizeof(int16_t) * sp->samprate; // fixed rate
	sp->frame_size = pkt->len / (sizeof(int16_t) * sp->channels); // mono/stereo samples in frame
	assert(bounce != NULL);
	if(encoding == S16BE){
	  int16_t const * const data = (int16_t *)&pkt->data[0];
	  for(int i=0; i < sp->channels * sp->frame_size; i++)
	    bounce[i] = SCALE16 * (int16_t)ntohs(data[i]); // Cast is necessary
	} else {
	  int16_t const * const data = (int16_t *)&pkt->data[0];
	  for(int i=0; i < sp->channels * sp->frame_size; i++)
	    bounce[i] = SCALE16 * data[i];
	}
	break;
      case F32LE:
	sp->datarate = 8 * sp->channels * sizeof(float) * sp->samprate;
	sp->frame_size = pkt->len / (sizeof(float) * sp->channels); // mono/stereo samples in frame
	assert(bounce != NULL);
	{
	  float const * const data = (float *)&pkt->data[0];
	  for(int i=0; i < sp->channels * sp->frame_size; i++)
	    bounce[i] = data[i];
	}
	break;
#ifdef HAS_FLOAT16
      case F16LE: // 16-bit floats
	sp->datarate = 8 * sp->channels * sizeof(float16_t) * sp->samprate;
	sp->frame_size = pkt->len / (sizeof(float16_t) * sp->channels); // mono/stereo samples in frame
	assert(bounce != NULL);
	{
	  float16_t const * const data = (float16_t *)&pkt->data[0];
	  for(int i=0; i < sp->channels * sp->frame_size; i++)
	    bounce[i] = data[i];
	}
	break;
#endif
      default:
	sp->frame_size = 0; // Unknown size
	goto advance; // Unknown, ignore
      } // end of PCM switch
    } // end of PCM processing
    // we either have actual data, PLC samples or silence in the bounce buffer
    // accumulate seconds of real time for display
    sp->tot_active += (double)sp->frame_size / sp->samprate;
    sp->active += (double)sp->frame_size / sp->samprate;
    assert(bounce != NULL);
    
    // Run PL tone decoders on the bounce buffer. We don't really care about timestamp jumps
    // Disable if display isn't active and autonotching is off
    // Fed audio that might be discontinuous or out of sequence, but it's a pain to fix
    if(sp->notch_enable) {
      for(int i=0; i < sp->frame_size; i++){
	double const s = (sp->channels == 2) ? 0.5 * (bounce[2*i] + bounce[2*i+1]) : bounce[i];
	
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
	  double energy = cnrm(output_goertzel(&sp->tone_detector[j]));
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
    } // End of PL tone decoding
    // end of decoded buffer processing (real, concealed or silence)
    // Thumping artifacts during vote switching seem worse if we bail out here, so we keep the tone notch filters going
    // on out-voted channels
    if(sp->muted)
      goto advance;

    // Apply notch filter, if enabled
    // Do this even when not selected by voting, to prevent transients when it's selected
    if(sp->notch_enable && sp->notch_tone > 0){
      if(sp->channels == 1){
	for(int i = 0; i < sp->frame_size; i++)
	  bounce[i] = (float)applyIIR(&sp->iir_left,bounce[i]);
      } else {
	for(int i = 0; i < sp->frame_size; i++){
	  bounce[2*i] = (float)applyIIR(&sp->iir_left,bounce[2*i]);
	  bounce[2*i+1] = (float)applyIIR(&sp->iir_right,bounce[2*i+1]);
	}
      }
    } // End of tone notching

    if(Voting && Best_session != sp)
      goto advance;  // If voting, suppress all but best session

    float *output_rate_data = NULL;
    int output_rate_data_size = 0;
    if(upsample_ratio != 1){
      int error;
      if(sp->channels == 1 && src_state_mono == NULL){
	src_state_mono = src_new(SRC_SINC_FASTEST, 1, &error);
	assert(src_state_mono != NULL);
      }
      else if(sp->channels == 2 && src_state_stereo == NULL){
	src_state_stereo = src_new(SRC_SINC_FASTEST, 2, &error);
	assert(src_state_stereo != NULL);
      }
      // Create or enlarge rate converter output buffer
      int const output_size = maxsamples * 2 * DAC_samprate;
      if(rate_converted_buffer == NULL || rate_converted_buffer_size < output_size){
	rate_converted_buffer_size = output_size;
	rate_converted_buffer = malloc(rate_converted_buffer_size);
      }
      assert(rate_converted_buffer != NULL);
      SRC_DATA src_data;
      src_data.data_in = bounce;  // Pointer to input audio
      src_data.data_out = rate_converted_buffer;  // Pointer to resampled output buffer
      src_data.input_frames = sp->frame_size;
      src_data.output_frames = (int)ceil(sp->frame_size * upsample_ratio + 1);
      src_data.src_ratio = (double)upsample_ratio;
      src_data.end_of_input = 0;
      if(sp->channels == 1)
	error = src_process(src_state_mono, &src_data);
      else
	error = src_process(src_state_stereo, &src_data);
      assert(error == 0);
      output_rate_data = rate_converted_buffer;
      output_rate_data_size = (int)round(sp->frame_size * upsample_ratio);
    } else {
      // upsample_ratio == 1; just pass a pointer to the input
      output_rate_data = bounce;
      output_rate_data_size = sp->frame_size;
    }
    // Figure out where to sum into output buffer
    // We didn't actully know how many samples we have to write until after decoding
    int wptr = sp->wptr + time_diff;
    if(wptr < 0)
      wptr += BUFFERSIZE;
    if(wptr >= BUFFERSIZE)
      wptr -= BUFFERSIZE;
    int const rptr = atomic_load_explicit(&Rptr,memory_order_relaxed);
    if(modsub(wptr,rptr,BUFFERSIZE) < output_rate_data_size){
      // oops not enough room; should we keep what fits?
      sp->lates++;
      if(++consec_lates < 6)
	goto advance; // drop it on the floor for now

      // A playout reset should make room
      consec_lates = 0;
      reset_playout(sp);
      time_diff = 0;
      wptr = sp->wptr;
    }
    // Mix output sample rate data into output ring buffer
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
      
      // Mix into output buffer read by portaudio callback
      // Simplified by mirror buffer wrap
      int left_index = Channels * (wptr + left_delay);
      int right_index = Channels * (wptr + right_delay) + 1;
      
      if(sp->channels == 1){
	for(int i=0; i < output_rate_data_size; i++){
	  // Mono input, put on both channels
	  double s = output_rate_data[i];
	  double tl = Output_buffer[left_index] + s * left_gain;
	  Output_buffer[left_index] = (float)tl;
	  double tr = Output_buffer[right_index] + s * right_gain;
	  Output_buffer[right_index] = (float)tr;
	  left_index += Channels;
	  right_index += Channels;
	  if(modsub(right_index/2,Wptr,BUFFERSIZE) > 0)
	    Wptr = right_index / 2; // samples to frames; For verbose mode
	}
      } else {
	for(int i=0; i < output_rate_data_size; i++){
	  // stereo input
	  double left = output_rate_data[2*i];
	  double right = output_rate_data[2*i+1];
	  double tl = Output_buffer[left_index] + left * left_gain;
	  Output_buffer[left_index] = (float)tl;
	  double tr = Output_buffer[right_index] + right * right_gain;
	  Output_buffer[right_index] = (float)tr;
	  left_index += Channels;
	  right_index += Channels;
	  if(modsub(right_index/2,Wptr,BUFFERSIZE) > 0)
	    Wptr = right_index / 2; // samples to frames; For verbose mode
	}
      }
    } else { // Channels == 1, no panning
      if(sp->channels == 1){
	int index = wptr;
	for(int i=0; i < output_rate_data_size; i++){
	  double s = sp->gain * output_rate_data[i] + Output_buffer[index];
	  Output_buffer[index++] = (float)s;
	  if(modsub(index,Wptr,BUFFERSIZE) > 0)
	    Wptr = index; // For verbose mode
	}
      } else {  // sp->channels == 2
	int index = wptr;
	for(int i=0; i < output_rate_data_size; i++){
	  // Downmix to mono
	  double s = Output_buffer[index] + 0.5 * Gain * (output_rate_data[2*i] + output_rate_data[2*i+1]);
	  Output_buffer[index++] = (float)s;
	  if(modsub(index,Wptr,BUFFERSIZE) > 0)
	    Wptr = index; // For verbose mode
	}
      } // if(sp->channels == 1)
    } // if(Channels == 2)
    // End of output mixing; update write pointer even if we didn't actually write
  advance:;
    // Use this target only pkt != NULL
    // We've written something to the output buffer.
    // Advance the write pointer and the expected timestamp
    // Adjust for any jump in the timestamp
    sp->next_timestamp = (uint32_t)(pkt->rtp.timestamp + sp->frame_size + time_diff);
  advance_out:; // Advance the output pointer only (eg, after PLC)
    sp->wptr += (int)round(upsample_ratio * (sp->frame_size + time_diff));
    sp->wptr &= (BUFFERSIZE-1);
  endloop:;
    FREE(pkt); // Free packet if it exists
  } // !sp->terminate
  FREE(bounce);
  FREE(rate_converted_buffer);
  if(src_state_mono)
    src_delete(src_state_mono);
  if(src_state_stereo)
    src_delete(src_state_stereo);
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
  int const rptr = atomic_load_explicit(&Rptr,memory_order_relaxed);
  sp->wptr = (rptr + sp->playout) & (BUFFERSIZE-1);
}
// Start output stream if it was off; reset idle timeout on output audio stream activity
// Return true if we (re)started it
bool kick_output(){
  bool restarted = false;
  if(!Pipe && !Pa_IsStreamActive(Pa_Stream)){
    // Start it up
    if(!Pa_IsStreamStopped(Pa_Stream))
      Pa_StopStream(Pa_Stream); // it was in limbo

    Start_time = gps_time_ns();
    Start_pa_time = Pa_GetStreamTime(Pa_Stream); // Stream Time runs continuously even when stream stopped
    Audio_frames = 0;
    // Adjust Rptr for the missing time we were asleep, but only
    // if this isn't the first time
    // This will break if someone goes back in time and starts this program at precisely 00:00:00 UTC on 1 Jan 1970 :-)
    if(Last_callback_time != 0){
      int rptr = atomic_load_explicit(&Rptr,memory_order_relaxed);
      rptr += (int)round(DAC_samprate * (Start_pa_time - Last_callback_time));
      rptr &= (BUFFERSIZE-1);
      atomic_store_explicit(&Rptr,rptr,memory_order_release);
    }

    int r = Pa_StartStream(Pa_Stream); // Immediately triggers the first callback
    if(r != paNoError){
      fprintf(stderr,"Portaudio error: %s, aborting\n",Pa_GetErrorText(r));
      abort();
    }
    restarted = true;
  }
  atomic_store(&Buffer_length,BUFFERSIZE); // (Continue to) run for at least the length of the ring buffer

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

#if 0
// How far ahead of the output we are, in frames
static int buffer_margin(struct session const *sp){
  int rptr = atomic_load_explicit(&Rptr,memory_order_acquire);
  return modsub(sp->wptr,rptr,BUFFERSIZE);
}
#endif
