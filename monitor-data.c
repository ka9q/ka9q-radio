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
#if __linux__
#include <bsd/string.h>
#include <alsa/asoundlib.h>
#else
#include <string.h>
#endif
#include <sysexits.h>
#include <poll.h>

#include "conf.h"
#include "config.h"
#include "misc.h"
#include "multicast.h"
#include "radio.h"
#include "iir.h"
#include "morse.h"
#include "status.h"
#include "monitor.h"

int Position; // auto-position streams
int Invalids;

// All the tones from various groups, including special NATO 150 Hz tone
float PL_tones[] = {
     67.0,  69.3,  71.9,  74.4,  77.0,  79.7,  82.5,  85.4,  88.5,  91.5,
     94.8,  97.4, 100.0, 103.5, 107.2, 110.9, 114.8, 118.8, 123.0, 127.3,
    131.8, 136.5, 141.3, 146.2, 150.0, 151.4, 156.7, 159.8, 162.2, 165.5,
    167.9, 171.3, 173.8, 177.3, 179.9, 183.5, 186.2, 189.9, 192.8, 196.6,
    199.5, 203.5, 206.5, 210.7, 213.8, 218.1, 221.3, 225.7, 229.1, 233.6,
    237.1, 241.8, 245.5, 250.3, 254.1
};
static float make_position(int x);


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
    struct sockaddr sock;
    resolve_mcast(mcast_address_text,&sock,DEFAULT_RTP_PORT,iface,sizeof(iface),0);
    input_fd = listen_mcast(&sock,iface);
  }
  if(input_fd == -1)
    pthread_exit(NULL);

  struct packet *pkt = NULL;

  realtime();
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
    int size = recvfrom(input_fd,&pkt->content,sizeof(pkt->content),0,(struct sockaddr *)&sender,&socksize);
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
      sp->gain = powf(10.,0.05 * Gain);    // Start with global default
      sp->notch_enable = Notch;
      sp->muted = Start_muted;
      sp->dest = mcast_address_text;
      sp->last_timestamp = pkt->rtp.timestamp;
      sp->rtp_state.seq = pkt->rtp.seq;
      sp->reset = true;
      sp->init = true;

      if(pthread_create(&sp->task,NULL,decode_task,sp) == -1){
	perror("pthread_create");
	close_session(&sp);
	continue;
      }
    }
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
    long long t = gps_time_ns();
    if(t - sp->last_active > BILLION){
      // Transition from idle to active
      sp->last_start = t;
    }
    sp->last_active = t;
    // wake up decoder thread
    pthread_cond_signal(&sp->qcond);
    pthread_mutex_unlock(&sp->qmutex);
  }
  return NULL;
}
void decode_task_cleanup(void *arg){
  struct session *sp = (struct session *)arg;
  assert(sp);

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
  struct frontend * const frontend = &sp->frontend;
  FREE(frontend->description);

  // Just in case anything was allocated for these arrays
  struct channel * const chan = &sp->chan;
  FREE(chan->filter.energies);
  FREE(chan->spectrum.bin_data);
  FREE(chan->status.command);
}

// Per-session thread to decode incoming RTP packets
// Not needed for PCM, but Opus can be slow
void *decode_task(void *arg){
  struct session *sp = (struct session *)arg;
  assert(sp);

  {
    char name[100];
    snprintf(name,sizeof(name),"dec %u",sp->ssrc);
    pthread_setname(name);
  }
  pthread_cleanup_push(decode_task_cleanup,arg); // called on termination

  int consec_lates = 0;
  int consec_earlies = 0;
  float *bounce = NULL;

  // Main loop; run until asked to quit
  while(!sp->terminate && !Terminate){
    struct packet *pkt = NULL;
    // Wait for packet to appear on queue
    pthread_mutex_lock(&sp->qmutex);
    while(!sp->queue){
      int64_t const increment = 100000000; // 100 ms
      // pthread_cond_timedwait requires UTC clock time! Undefined behavior around a leap second...
      struct timespec ts;
      ns2ts(&ts,utc_time_ns() + increment);
      int r = pthread_cond_timedwait(&sp->qcond,&sp->qmutex,&ts); // Wait 100 ms max so we pick up terminates
      if(r != 0){
	if(r == EINVAL)
	  Invalids++;
	pthread_mutex_unlock(&sp->qmutex);
	goto endloop;// restart loop, checking terminate flags
      }
    }
    // Peek at first packet on queue; is it in sequence?
    if(sp->queue->rtp.seq != sp->rtp_state.seq){
      // No. If we've got plenty in the playout buffer, sleep to allow some packet resequencing in the input thread.
      // Strictly speaking, we will resequence ourselves below with the RTP timestamp. But that works properly only with stateless
      // formats like PCM. Opus is stateful, so it's better to resequence input packets (using the RTP sequence #) when possible.
      float queue = (float)modsub(sp->wptr,Rptr,BUFFERSIZE) / DAC_samprate;
      if(queue > Latency + 0.1){ // 100 ms for scheduling latency?
	pthread_mutex_unlock(&sp->qmutex);
	struct timespec ss;
	ns2ts(&ss,(int64_t)(1e9 * (queue - (Latency + 0.1))));
	nanosleep(&ss,NULL);
	goto endloop;
      }
      // else the playout queue is close to draining, accept out of sequence packet anyway
    }
    pkt = sp->queue;
    sp->queue = pkt->next;
    pkt->next = NULL;
    pthread_mutex_unlock(&sp->qmutex);

    sp->packets++; // Count all packets, regardless of type
    if((int16_t)(pkt->rtp.seq - sp->rtp_state.seq) > 0){ // Doesn't really handle resequencing
      if(!pkt->rtp.marker){
	sp->rtp_state.drops++; // Avoid spurious drops when session is recreated after silence
	Last_error_time = gps_time_ns();
      }
      if(sp->opus)
	opus_decoder_ctl(sp->opus,OPUS_RESET_STATE); // Reset decoder when there's a jump
    }
    sp->rtp_state.seq = pkt->rtp.seq + 1; // Expect the next seq # next time

    if(!sp->muted && pkt->rtp.marker){
      // beginning of talk spurt, resync
      reset_session(sp,pkt->rtp.timestamp); // Updates sp->wptr
    }
    if(pkt->rtp.type < 128)
      sp->type = pkt->rtp.type; // Save only if valid

    enum encoding const encoding = sp->pt_table[sp->type].encoding;
    if(encoding == NO_ENCODING || encoding == AX25)
      goto endloop;
    
    int upsample;

    // The Opus decoder is always forced to the local channel count because the input stream can switch at any time
    // (e.g., I/Q vs envelope) without changing the payload type, so there could be a glitch
    // before the channel count in the status message catches up with us and we can initialize a new decoder
    if(encoding == OPUS){
      sp->samprate = DAC_samprate;
      sp->channels = Channels;
      upsample = 1;
      if(!sp->opus){
	// This should happen only once on a stream
	// Always decode Opus to DAC rate of 48 kHz, stereo
	int error;
	sp->opus = opus_decoder_create(sp->samprate,sp->channels,&error);
	if(error != OPUS_OK)
	  fprintf(stderr,"opus_decoder_create error %d\n",error);

	assert(sp->opus);
	// Init PL tone detectors
	for(int j=0; j < N_tones; j++)
	  init_goertzel(&sp->tone_detector[j],PL_tones[j]/(float)sp->samprate);
	sp->notch_tone = 0;
      }
      int const r1 = opus_packet_get_nb_samples(pkt->data,pkt->len,sp->samprate);
      if(r1 == OPUS_INVALID_PACKET || r1 == OPUS_BAD_ARG)
	goto endloop;

      assert(r1 >= 0);
      sp->frame_size = r1;
      int const r2 = opus_packet_get_bandwidth(pkt->data);
      if(r2 == OPUS_INVALID_PACKET || r2 == OPUS_BAD_ARG)
	goto endloop;
      switch(r2){
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
      sp->opus_channels = opus_packet_get_nb_channels(pkt->data);
      size_t const bounce_size = sizeof(*bounce) * sp->frame_size * sp->channels;
      assert(bounce == NULL); // detect possible memory leaks
      bounce = malloc(bounce_size);
      int const samples = opus_decode_float(sp->opus,pkt->data,pkt->len,bounce,bounce_size,0);
      if(samples != sp->frame_size){
	fprintf(stderr,"samples %d frame-size %d\n",samples,sp->frame_size);
	goto endloop;
      }
      // Maintain smoothed measurement of data rate
      // Won't work right with discontinuous transmission - fix by looking at timestamps
      float rate = 8 * pkt->len * DAC_samprate / (float)samples;
      sp->datarate += 0.1 * (rate - sp->datarate);
    } else { // PCM
      sp->channels = sp->pt_table[sp->type].channels;
      int samprate = sp->pt_table[sp->type].samprate;
      if(samprate <= 0 || sp->channels <= 0 || sp->channels > 2)
	goto endloop;

      if(samprate != sp->samprate){
	// Reinit tone detectors whenever sample rate changes
	sp->samprate = samprate;
	for(int j=0; j < N_tones; j++)
	  init_goertzel(&sp->tone_detector[j],PL_tones[j]/(float)sp->samprate);
	sp->notch_tone = 0; // force it to be re-detected at new sample rate
	sp->bandwidth = samprate / 2000;    // in kHz allowing for Nyquist, using actual input sample rate for Opus
      }
      // Upsample (PCM only) lower samprates to output rate
      // (should be cleaner; what about decimation?)
      upsample = DAC_samprate / sp->samprate;


      // decode PCM into bounce buffer
      switch(encoding){
      case S16LE:
      case S16BE:
	sp->datarate = 8 * sp->channels * sizeof(int16_t) * sp->samprate;
	sp->frame_size = pkt->len / (sizeof(int16_t) * sp->channels); // mono/stereo samples in frame
	if(sp->frame_size <= 0)
	  goto endloop;
	assert(bounce == NULL);
	bounce = malloc(sizeof(*bounce) * sp->frame_size * sp->channels);
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
	if(sp->frame_size <= 0) // Check here because it might truncate to zero
	  goto endloop;
	assert(bounce == NULL);
	bounce = malloc(sizeof(*bounce) * sp->frame_size * sp->channels);
	{
	  float const * const data = (float *)&pkt->data[0];
	  for(int i=0; i < sp->channels * sp->frame_size; i++)
	    bounce[i] = data[i];
	}
	break;
#ifdef FLOAT16
      case F16LE: // 16-bit floats
	sp->datarate = 8 * sp->channels * sizeof(_Float16) * sp->samprate;
	sp->frame_size = pkt->len / (sizeof(_Float16) * sp->channels); // mono/stereo samples in frame
	if(sp->frame_size <= 0) // Check here because it might truncate to zero
	  goto endloop;
	assert(bounce == NULL);
	bounce = malloc(sizeof(*bounce) * sp->frame_size * sp->channels);
	{
	  _Float16 const * const data = (_Float16 *)&pkt->data[0];
	  for(int i=0; i < sp->channels * sp->frame_size; i++)
	    bounce[i] = data[i];
	}
	break;
#endif
      default:
	goto endloop; // Unknown, ignore
      } // end of PCM switch
    }
    // Run PL tone decoders
    // Disable if display isn't active and autonotching is off
    // Fed audio that might be discontinuous or out of sequence, but it's a pain to fix
    if(sp->notch_enable) {
      for(int i=0; i < sp->frame_size; i++){
	float s;
	if(sp->channels == 2)
	  s = 0.5 * (bounce[2*i] + bounce[2*i+1]); // Mono sum
	else // sp->channels == 1
	  s = bounce[i];

	for(int j = 0; j < N_tones; j++)
	  update_goertzel(&sp->tone_detector[j],s);
      }
      sp->tone_samples += sp->frame_size;
      if(sp->tone_samples >= Tone_period * sp->samprate){
	sp->tone_samples = 0;
	int pl_tone_index = -1;
	float strongest_tone_energy = 0;
	float total_energy = 0;
	for(int j=0; j < N_tones; j++){
	  float energy = cnrmf(output_goertzel(&sp->tone_detector[j]));
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
	char const *id = lookupid(sp->chan.tune.freq,sp->current_tone); // Any or no tone
	if(id)
	  strlcpy(sp->id,id,sizeof(sp->id));
	else
	  sp->id[0] = '\0';
      }
    }
    if(strlen(sp->id) == 0){
      char const *id = lookupid(sp->chan.tune.freq,0.0);
      if(id)
	strlcpy(sp->id,id,sizeof(sp->id));
    }
    // Count samples and frames and advance write pointer even when muted
    sp->tot_active += (float)sp->frame_size / sp->samprate;
    sp->active += (float)sp->frame_size / sp->samprate;

    kick_output(); // Ensure Rptr is current
    // Sequence number processing and write pointer updating
    if(sp->reset){
      reset_session(sp,pkt->rtp.timestamp); // Resets sp->wptr and last_timestamp
    } else if(modsub(sp->wptr,Rptr,BUFFERSIZE) < 0){
      sp->lates++;
      if(++consec_lates < 3 || Constant_delay)
	goto endloop;
      // 3 or more consecutive lates triggers a reset, unless constant delay is selected
      reset_session(sp,pkt->rtp.timestamp);
    } else if(modsub(sp->wptr,Rptr,BUFFERSIZE) > BUFFERSIZE/4){
      sp->earlies++;
      if(++consec_earlies < 3)
	goto endloop;
      reset_session(sp,pkt->rtp.timestamp);
    }
    consec_lates = 0;
    consec_earlies = 0;

    // Normal packet, relative adjustment to write pointer
    // Can difference in timestamps be negative? Cast it anyway
    // Opus always counts timestamps at 48 kHz so this breaks when DAC_samprate is not 48 kHz
    // For opus, sp->wptr += (int32_t)(pkt->rtp.timestamp - sp->last_timestamp) * DAC_samprate / 48000;
    sp->wptr += (int32_t)(pkt->rtp.timestamp - sp->last_timestamp) * upsample;
    sp->wptr &= (BUFFERSIZE-1);
    sp->last_timestamp = pkt->rtp.timestamp;

    vote();
    // Skip output if session is muted
    // Thumping artifacts during vote switching seem worse if we bail out here, so we keep the tone notch filters going
    // on out-voted channels
    if(sp->muted)
      goto endloop; // No more to do with this frame

    if(Channels == 2){
      /* Compute gains and delays for stereo imaging
	 Extreme gain differences can make the source sound like it's inside an ear
	 This can be uncomfortable in good headphones with extreme panning
	 -6dB for each channel in the center
	 when full to one side or the other, that channel is +6 dB and the other is -inf dB
      */
      float const left_gain = sp->gain * (1 - sp->pan)/2;
      float const right_gain = sp->gain * (1 + sp->pan)/2;
      /* Delay less favored channel 0 - 1.5 ms max (determined
	 empirically) This is really what drives source localization
	 in humans. The effect is so dramatic even with equal levels
	 you have to remove one earphone to convince yourself that the
	 levels really are the same!
      */
      int const left_delay = (sp->pan > 0) ? round(sp->pan * .0015 * DAC_samprate) : 0; // Delay left channel
      int const right_delay = (sp->pan < 0) ? round(-sp->pan * .0015 * DAC_samprate) : 0; // Delay right channel

      assert(left_delay >= 0 && right_delay >= 0);

      // Mix bounce buffer into output buffer read by portaudio callback
      // Simplified by mirror buffer wrap
      int left_index = 2 * (sp->wptr + left_delay);
      int right_index = 2 * (sp->wptr + right_delay) + 1;

      for(int i=0; i < sp->frame_size; i++){
	float left,right;
	if(sp->channels == 1){
	  // Mono input, put on both channels
	  left = bounce[i];
	  if(sp->notch_enable && sp->notch_tone > 0)
	    left = applyIIR(&sp->iir_left,left);
	  right = left;
	} else {
	  // stereo input
	  left = bounce[2*i];
	  right = bounce[2*i+1];
	  if(sp->notch_enable && sp->notch_tone > 0){
	    left = applyIIR(&sp->iir_left,left);
	    right = applyIIR(&sp->iir_right,right);
	  }
	}
	if(!Voting || Best_session == sp){ // If voting, suppress all but best session
	  // Not the cleanest way to upsample the sample rate, but it works
	  // Should be replaced with a proper interpolator
	  for(int j=0; j < upsample; j++){
	    Output_buffer[left_index] += left * left_gain;
	    Output_buffer[right_index] += right * right_gain;
	    left_index += 2;
	    right_index += 2;
	  }
	  if(modsub(right_index/2,Wptr,BUFFERSIZE) > 0)
	    Wptr = right_index / 2; // samples to frames; For verbose mode
	}
      }
    } else { // Channels == 1, no panning
      int64_t index = sp->wptr;
      for(int i=0; i < sp->frame_size; i++){
	float s;
	if(sp->channels == 1){
	  s = bounce[i];
	} else {
	  // Downmix to mono
	  s = 0.5 * (bounce[2*i] + bounce[2*i+1]);
	}
	if(sp->notch_enable && sp->notch_tone > 0)
	  s = applyIIR(&sp->iir_left,s);

	if(!Voting || Best_session == sp){ // If voting, suppress all but best session
	  // Not the cleanest way to upsample the sample rate, but it works
	  for(int j=0; j < upsample; j++)
	    Output_buffer[index++] += s * sp->gain;

	  if(modsub(index,Wptr,BUFFERSIZE) > 0)
	    Wptr = index; // For verbose mode
	}
      }
    } // Channels == 1

  endloop:;
    FREE(bounce);
    FREE(pkt);
  } // !sp->terminate
  pthread_cleanup_pop(1);
  return NULL;
}

void reset_session(struct session * const sp,uint32_t timestamp){
  sp->resets++;
  if(sp->opus)
    opus_decoder_ctl(sp->opus,OPUS_RESET_STATE); // Reset decoder
  sp->reset = false;
  sp->last_timestamp = timestamp;
  sp->playout = Playout * DAC_samprate/1000;
  sp->wptr = (Rptr + sp->playout) & (BUFFERSIZE-1);
}
// Start output stream if it was off; reset idle timeout on output audio stream activity
// Return true if we (re)started it
bool kick_output(){
  bool restarted = false;
  if(!Pa_IsStreamActive(Pa_Stream)){
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
      Rptr += DAC_samprate * (Start_pa_time - Last_callback_time);
      Rptr &= (BUFFERSIZE-1);
    }

    int r = Pa_StartStream(Pa_Stream); // Immediately triggers the first callback
    if(r != paNoError){
      fprintf(stderr,"Portaudio error: %s, aborting\n",Pa_GetErrorText(r));
      abort();
    }
    restarted = true;
  }
  Buffer_length = BUFFERSIZE; // (Continue to) run for at least the length of the ring buffer

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
static float make_position(int x){
  x += 1; // Force first position to be in center, which is the default with a single stream
  // Swap bit order
  int y = 0;
  const int w = 8;
  for(int i=0; i < w; i++){
    y = (y << 1) | (x & 1);
    x >>= 1;
  }
  // Scale
  return 0.5 * (((float)y / 128) - 1);
}
