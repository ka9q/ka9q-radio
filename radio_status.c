// Generate UDP status messages from radiod, accept incoming commands to radiod in same format
// Copyright 2023 Phil Karn, KA9Q

#define _GNU_SOURCE 1
#include <assert.h>

#include <stdint.h>
#include <unistd.h>
#include <limits.h>
#include <pthread.h>
#include <string.h>
#include <pthread.h>
#if defined(linux)
#include <bsd/string.h>
#include <bsd/stdlib.h>
#else
#include <stdlib.h>
#endif
#include <math.h>
#include <complex.h>
#undef I
#include <sys/time.h>
#include <ctype.h>
#include <sys/socket.h>
#include <netdb.h>
#include <errno.h>

#include "misc.h"
#include "radio.h"
#include "filter.h"
#include "multicast.h"
#include "status.h"

int Status_fd;  // File descriptor for receiver status
int Ctl_fd;     // File descriptor for receiving user commands


extern dictionary const *Preset_table;

static int send_radio_status(struct frontend *frontend,struct channel *chan);
static int decode_radio_commands(struct channel *chan,uint8_t const *buffer,int length);
static int encode_radio_status(struct frontend const *frontend,struct channel const *chan,uint8_t *packet, int len);
static void *radio_status_dump(void *);
  
static pthread_t status_dump_thread;
static pthread_cond_t Status_dump_cond;
static pthread_mutex_t Status_dump_mutex;
static bool Status_kick_flag;
static uint32_t Tag;

// Radio status reception and transmission thread
void *radio_status(void *arg){

  char name[100];
  snprintf(name,sizeof(name),"radio stat");
  pthread_setname(name);
  
  pthread_create(&status_dump_thread,NULL,radio_status_dump,NULL);

  while(1){
    // Command from user
    uint8_t buffer[8192];
    int const length = recv(Ctl_fd,buffer,sizeof(buffer),0);
    if(length <= 0 || (enum pkt_type)buffer[0] != CMD)
      continue; // short packet, or a response; ignore

    // for a specific ssrc?
    uint32_t ssrc = get_ssrc(buffer+1,length-1);
    switch(ssrc){
    case 0:
      // Ignore; reserved for dynamic channel template
      break;
    case 0xffffffff:
      // Kick separate thread to dump all chans in a rate-limited manner
      pthread_mutex_lock(&Status_dump_mutex);
      Status_kick_flag = true;
      Tag = get_tag(buffer+1,length-1);
      pthread_cond_signal(&Status_dump_cond);
      pthread_mutex_unlock(&Status_dump_mutex);
      break;
    default:
      {
	// find specific chan instance
	struct channel *chan = lookup_chan(ssrc);
	if(chan == NULL){
	  if((chan = setup_chan(ssrc)) == NULL){ // possible race here?
	    // Creation failed, e.g., no output stream
	    fprintf(stdout,"Dynamic create of ssrc %u failed; is 'data =' set in [global]?\n",ssrc);
	    continue;
	  } else {
	    start_demod(chan);
	    if(Verbose)
	      fprintf(stdout,"dynamically started ssrc %u\n",ssrc);
	  }
	}
	// chan now != NULL
	if(chan->lifetime != 0)
	  chan->lifetime = 20; // Restart 20 second self-destruct timer
	chan->commands++;
	decode_radio_commands(chan,buffer+1,length-1);
	send_radio_status(&Frontend,chan); // Send status in response
      }
      break;
    }
  }
  return NULL;
}

// Dump all statuses, with rate limiting
static void *radio_status_dump(void *p){
  pthread_setname("statusdump");
  pthread_detach(pthread_self());

  while(1){
    // Wait to be notified by radio_status thread that we've received a request to
    // dump the entire channel table.
    pthread_mutex_lock(&Status_dump_mutex);
    while(Status_kick_flag == false)
      pthread_cond_wait(&Status_dump_cond,&Status_dump_mutex);
    Status_kick_flag = false;
    pthread_mutex_unlock(&Status_dump_mutex);

    // Walk through the channel table dumping each status
    // Wait a random time before continuing to avoid flooding the net with back-to-back packets
    // This is why we have to be a separate thread
    // While the list can change while we're walking it, this isn't terribly serious
    // because it's a table, not a linked list. At the worst we emit a stale entry or miss a new one
    // Otherwise we
    for(int i=0; i < Channel_list_length; i++){
      struct channel *chan = &Channel_list[i];
      if(!chan->inuse)
	continue;
      if(chan->output.rtp.ssrc == 0xffffffff || chan->output.rtp.ssrc == 0)
	continue; // Reserved for dynamic channel template or all-call polling
      chan->commands++;
      chan->command_tag = Tag;
      send_radio_status(&Frontend,chan);

      // Rate limit to 200/sec on average by randomly delaying 0-10 ms
      struct timespec sleeptime;
      sleeptime.tv_sec = 0;
      sleeptime.tv_nsec = arc4random_uniform(10000000); // 10 million ns
      nanosleep(&sleeptime,NULL);
    }
  }
  return NULL;
}


static int send_radio_status(struct frontend *frontend,struct channel *chan){
  uint8_t packet[PKTSIZE];

  Metadata_packets++;
  int const len = encode_radio_status(frontend,chan,packet,sizeof(packet));
  send(Status_fd,packet,len,0);
  // Reset integrators
  chan->sig.bb_energy = 0;
  chan->output.energy = 0;
  chan->output.sum_gain_sq = 0;
  chan->blocks_since_poll = 0;
  return 0;
}


// Decode and save incoming status from either SDR front end or from radio program
// from indicates source of message: from SDR front end or from radio program
// cmd == 1 means this is a command, only allow certain items

// with SSRC selection, should scan entire command for our SSRC before we execute any of it
static int decode_radio_commands(struct channel *chan,uint8_t const *buffer,int length){
  bool restart_needed = false;
  bool new_filter_needed = false;
  uint32_t const ssrc = chan->output.rtp.ssrc;
  
  uint8_t const *cp = buffer;
  while(cp - buffer < length){
    enum status_type type = *cp++; // increment cp to length field

    if(type == EOL)
      break; // end of list, no length

    unsigned int optlen = *cp++;
    if(optlen & 0x80){
      // length is >= 128 bytes; fetch actual length from next N bytes, where N is low 7 bits of optlen
      int length_of_length = optlen & 0x7f;
      optlen = 0;
      while(length_of_length > 0){
	optlen <<= 8;
	optlen |= *cp++;
	length_of_length--;
      }
    }
    if(cp - buffer + optlen >= length)
      break; // invalid length; we can't continue to scan

    switch(type){
    case EOL: // Shouldn't get here
      goto done;
    case COMMAND_TAG:
      chan->command_tag = decode_int32(cp,optlen);
      break;
    case OUTPUT_SAMPRATE:
      // Restart the demodulator to recalculate filters, etc
      {
	int new_sample_rate = decode_int(cp,optlen);
	if(new_sample_rate != chan->output.samprate){
	  chan->output.samprate = new_sample_rate;
	  restart_needed = true;
	}
      }
      break;
    case RADIO_FREQUENCY: // Hz
      {
	double const f = fabs(decode_double(cp,optlen));
	if(isfinite(f)){
	  if(f != chan->tune.freq && chan->demod_type == SPECT_DEMOD)
	    restart_needed = true; // Easier than trying to handle it inline
	  
	  if(Verbose > 1)
	    fprintf(stdout,"set ssrc %u freq = %.3lf\n",ssrc,f);

	  set_freq(chan,f);
	}
      }
      break;
    case FIRST_LO_FREQUENCY:
      {
	double const f = fabs(decode_double(cp,optlen));
	if(isfinite(f) && f != 0)
	  set_first_LO(chan,f); // Will ignore it if there's no change
      }
      break;
    case SECOND_LO_FREQUENCY: // Hz
      break; // No longer directly settable; change the first LO to do it indirectly
    case SHIFT_FREQUENCY: // Hz
      {
	double const f = decode_double(cp,optlen);
	if(isfinite(f))
	  chan->tune.shift = f;
      }
      break;
    case DOPPLER_FREQUENCY: // Hz
      {
	double const f = decode_double(cp,optlen);
	if(isfinite(f))
	  chan->tune.doppler = f;
      }
      break;
    case DOPPLER_FREQUENCY_RATE: // Hz
      {
	double const f = decode_double(cp,optlen);
	if(isfinite(f))
	  chan->tune.doppler_rate = f;
      }
      break;
    case LOW_EDGE: // Hz
      {
	float const f = decode_float(cp,optlen);
	if(isfinite(f) && f != chan->filter.min_IF){
	  chan->filter.min_IF = f;
	  new_filter_needed = true;
	}
      }
      break;
    case HIGH_EDGE: // Hz
      {
	float const f = decode_float(cp,optlen);
	if(isfinite(f) && chan->filter.max_IF != f){
	  chan->filter.max_IF = f;
	  new_filter_needed = true;
	}
      }
      break;
      case KAISER_BETA: // dimensionless, always 0 or positive
        {
	  float const f = fabsf(decode_float(cp,optlen));
	  if(isfinite(f) && chan->filter.kaiser_beta != f)
	    chan->filter.kaiser_beta = f;
	  new_filter_needed = true;
	}
      break;
    case PRESET:
      {
	char *p = decode_string(cp,optlen);
	strlcpy(chan->preset,p,sizeof(chan->preset));
	FREE(p); // decode_string now allocs memory
	{
	  enum demod_type const old_type = chan->demod_type;
	  int const old_samprate = chan->output.samprate;
	  float const old_low = chan->filter.min_IF;
	  float const old_high = chan->filter.max_IF;
	  float const old_kaiser = chan->filter.kaiser_beta;
	  float const old_shift = chan->tune.shift;

	  if(Verbose > 1)
	    fprintf(stdout,"command loadpreset(ssrc=%u) mode=%s\n",ssrc,chan->preset);
	  if(loadpreset(chan,Preset_table,chan->preset) != 0){
	    if(Verbose)
	      fprintf(stdout,"command loadpreset(ssrc=%u) mode=%sfailed!\n",ssrc,chan->preset);
	    break;
	  }
	  if(old_shift != chan->tune.shift)
	    set_freq(chan,chan->tune.freq + chan->tune.shift - old_shift);
	  if(chan->filter.min_IF != old_low || chan->filter.max_IF != old_high || chan->filter.kaiser_beta != old_kaiser)
	    new_filter_needed = true;

	  if(chan->demod_type != old_type || chan->output.samprate != old_samprate)
	    restart_needed = true; // chan changed, ask for a restart
	}
      }
      break;
    case DEMOD_TYPE:
      {
	enum demod_type const i = decode_int(cp,optlen);
	if(i >= 0 && i < Ndemod && i != chan->demod_type){
	  chan->demod_type = i;
	  restart_needed = true;
	}
      }
      break;
    case INDEPENDENT_SIDEBAND: // bool
      chan->filter.isb = decode_int8(cp,optlen); // will reimplement someday
      break;
    case THRESH_EXTEND: // bool
      chan->fm.threshold = decode_int8(cp,optlen);
      break;
    case HEADROOM: // dB -> voltage, always negative dB
      {
	float const f = decode_float(cp,optlen);
	if(isfinite(f))
	  chan->output.headroom = dB2voltage(-fabsf(f));
      }
      break;
    case AGC_ENABLE: // bool
      chan->linear.agc = decode_int8(cp,optlen);
      break;
    case GAIN:
      {
	float const f = decode_float(cp,optlen);
	if(!isnan(f)){
	  chan->output.gain = dB2voltage(f); // -Inf = 0 gain is OK
	  chan->linear.agc = false; // Doesn't make sense to change gain and then have the AGC change it again
	}
      }
      break;
    case AGC_HANGTIME: // seconds -> blocktimes
      {
	float const f = decode_float(cp,optlen);
	if(isfinite(f))
	  chan->linear.hangtime = fabsf(f) / (.001 * Blocktime);
      }
      break;
    case AGC_RECOVERY_RATE: // dB/sec -> amplitude / block times, always positive
      {
	float const f = decode_float(cp,optlen);
	if(!isnan(f))
	  chan->linear.recovery_rate = dB2voltage(fabsf(f) * .001 * Blocktime);
      }
      break;
    case AGC_THRESHOLD: // dB -> amplitude
      {
	float const f = decode_float(cp,optlen);
	if(isfinite(f))
	  chan->linear.threshold = dB2voltage(-fabsf(f));
      }
      break;
    case PLL_ENABLE: // bool
      chan->linear.pll = decode_int8(cp,optlen);
      break;
    case PLL_BW:
      {
	float const f = decode_float(cp,optlen); // Always 0 or positive
	if(isfinite(f))
	  chan->linear.loop_bw = fabsf(f);
      }
      break;
    case PLL_SQUARE: // bool
      chan->linear.square = decode_int8(cp,optlen);
      break;
    case ENVELOPE: // bool
      chan->linear.env = decode_int8(cp,optlen);
      break;
    case OUTPUT_CHANNELS: // int
      {
	int const i = decode_int(cp,optlen);
	if(i == 1 || i == 2)
	  chan->output.channels = i;
      }
      break;
    case SQUELCH_OPEN:
      {
	float const x = decode_float(cp,optlen);
	if(!isnan(x))
	  chan->squelch_open = fabsf(dB2power(x));
      }	
      break;
    case SQUELCH_CLOSE:
      {
	float const x = decode_float(cp,optlen);
	if(!isnan(x))
	   chan->squelch_close = fabsf(dB2power(x));
      }	
      break;
    case NONCOHERENT_BIN_BW:
      {
	float const x = decode_float(cp,optlen);
	if(!isnan(x) && x != chan->spectrum.bin_bw){
	  chan->spectrum.bin_bw = x;
	  restart_needed = true;
	}
      }
      break;
    case BIN_COUNT:
      {
	int const x = decode_int(cp,optlen);
	if(x > 0 && x != chan->spectrum.bin_count){
	  chan->spectrum.bin_count = x;
	  restart_needed = true;
	}
      }
      break;
    default:
      break;
    }
    cp += optlen;
  }
 done:;
  if(restart_needed){
    if(Verbose > 1)
      fprintf(stdout,"terminating chan thread for ssrc %u\n",ssrc);
    // Stop chan
    chan->terminate = true;
    pthread_join(chan->demod_thread,NULL);
    chan->demod_thread = (pthread_t)0;
    chan->terminate = false;
  }
  if(new_filter_needed){
    // Set up new filter with chan possibly stopped
    if(chan->filter.out){
      if(Verbose > 1)
	fprintf(stdout,"new filer for chan %u: IF=[%.0f,%.0f], samprate %d, kaiser beta %.1f\n",
		ssrc, chan->filter.min_IF, chan->filter.max_IF,
		chan->output.samprate, chan->filter.kaiser_beta);
      // start_demod already sets up a new filter
      set_filter(chan->filter.out,chan->filter.min_IF/chan->output.samprate,
		 chan->filter.max_IF/chan->output.samprate,
		 chan->filter.kaiser_beta);
    }
  }    
  if(restart_needed){
    if(Verbose > 1)
      fprintf(stdout,"starting chan type %d for ssrc %u\n",chan->demod_type,ssrc);
    start_demod(chan);
  }
  return 0;
}
  
// Encode contents of frontend and chan structures as command or status packet
// packet argument must be long enough!!
// Convert values from internal to engineering units
static int encode_radio_status(struct frontend const *frontend,struct channel const *chan,uint8_t *packet, int len){
  memset(packet,0,len);
  uint8_t *bp = packet;

  *bp++ = STATUS; // 0 = status, 1 = command

  // parameters valid in all modes
  encode_int32(&bp,OUTPUT_SSRC,chan->output.rtp.ssrc); // Now used as channel ID, so present in all modes
  encode_int32(&bp,COMMAND_TAG,chan->command_tag); // at top to make it easier to spot in dumps
  encode_int64(&bp,CMD_CNT,chan->commands); // integer
  if(strlen(frontend->description) > 0)
    encode_string(&bp,DESCRIPTION,frontend->description,strlen(frontend->description));
  
  // Echo timestamp from source or locally (bit of a kludge, eventually will always be local)
  if(frontend->timestamp != 0)
    encode_int64(&bp,GPS_TIME,frontend->timestamp);
  else
    encode_int64(&bp,GPS_TIME,gps_time_ns());

  encode_int64(&bp,INPUT_SAMPLES,frontend->samples);  
  encode_int32(&bp,INPUT_SAMPRATE,frontend->samprate); // integer Hz
  encode_int32(&bp,FE_ISREAL,frontend->isreal ? true : false);
  encode_double(&bp,CALIBRATE,frontend->calibrate);
  encode_double(&bp,RF_GAIN,frontend->rf_gain);
  encode_double(&bp,RF_ATTEN,frontend->rf_atten);
  encode_int32(&bp,LNA_GAIN,frontend->lna_gain);
  encode_int32(&bp,MIXER_GAIN,frontend->mixer_gain);
  encode_int32(&bp,IF_GAIN,frontend->if_gain);
  encode_float(&bp,FE_LOW_EDGE,frontend->min_IF);
  encode_float(&bp,FE_HIGH_EDGE,frontend->max_IF);
  encode_int32(&bp,AD_BITS_PER_SAMPLE,frontend->bitspersample);

  // Tuning
  encode_double(&bp,RADIO_FREQUENCY,chan->tune.freq); // Hz
  encode_double(&bp,FIRST_LO_FREQUENCY,frontend->frequency); // Hz
  encode_double(&bp,SECOND_LO_FREQUENCY,chan->tune.second_LO); // Hz

  if(frontend->in){
    encode_int32(&bp,FILTER_BLOCKSIZE,frontend->in->ilen);
    encode_int32(&bp,FILTER_FIR_LENGTH,frontend->in->impulse_length);
  }
  if(chan->filter.out != NULL)
    encode_int32(&bp,FILTER_DROPS,chan->filter.out->block_drops);  // count
  
  // Adjust for A/D width
  // Level is absolute relative to A/D saturation, so +3dB for real vs complex
  if(chan->blocks_since_poll > 0){
    float level = frontend->if_power;
    level /= (1 << (frontend->bitspersample-1)) * (1 << (frontend->bitspersample-1));
    // Scale real signals up 3 dB so a rail-to-rail sine will be 0 dBFS, not -3 dBFS
    // Complex signals carry twice as much power, divided between I and Q
    if(frontend->isreal)
      level *= 2;
    encode_float(&bp,IF_POWER,power2dB(level));
  }
  encode_float(&bp,NOISE_DENSITY,power2dB(chan->sig.n0));

  // Modulation mode
  encode_byte(&bp,DEMOD_TYPE,chan->demod_type);
  {
    int len = strlen(chan->preset);
    if(len > 0 && len < sizeof(chan->preset))
      encode_string(&bp,PRESET,chan->preset,len);
  }
  // Mode-specific params
  switch(chan->demod_type){
  case LINEAR_DEMOD:
    encode_byte(&bp,PLL_ENABLE,chan->linear.pll); // bool
    if(chan->linear.pll){
      encode_float(&bp,FREQ_OFFSET,chan->sig.foffset);     // Hz; used differently in linear and fm
      encode_byte(&bp,PLL_LOCK,chan->linear.pll_lock); // bool
      encode_byte(&bp,PLL_SQUARE,chan->linear.square); //bool
      encode_float(&bp,PLL_PHASE,chan->linear.cphase); // radians
      encode_float(&bp,PLL_BW,chan->linear.loop_bw);   // hz
      // Relevant only when squelches are active
      encode_float(&bp,SQUELCH_OPEN,power2dB(chan->squelch_open));
      encode_float(&bp,SQUELCH_CLOSE,power2dB(chan->squelch_close));
    }
    encode_byte(&bp,ENVELOPE,chan->linear.env); // bool
    encode_double(&bp,SHIFT_FREQUENCY,chan->tune.shift); // Hz
    encode_byte(&bp,AGC_ENABLE,chan->linear.agc); // bool
    if(chan->linear.agc){
      encode_float(&bp,AGC_HANGTIME,chan->linear.hangtime*(.001 * Blocktime)); // samples -> sec
      encode_float(&bp,AGC_THRESHOLD,voltage2dB(chan->linear.threshold)); // amplitude -> dB
      encode_float(&bp,AGC_RECOVERY_RATE,voltage2dB(chan->linear.recovery_rate)/(.001*Blocktime)); // amplitude/block -> dB/sec
    }
#if 0
    encode_byte(&bp,INDEPENDENT_SIDEBAND,chan->filter.isb); // bool - maybe reimplement someday
#endif
    break;
  case FM_DEMOD:
    if(chan->fm.tone_freq != 0){
      encode_float(&bp,PL_TONE,chan->fm.tone_freq);
      encode_float(&bp,PL_DEVIATION,chan->fm.tone_deviation);
    }
  case WFM_DEMOD:  // Note fall-through from FM_DEMOD
    // Relevant only when squelches are active
    encode_float(&bp,FREQ_OFFSET,chan->sig.foffset);     // Hz; used differently in linear and fm
    encode_float(&bp,SQUELCH_OPEN,power2dB(chan->squelch_open));
    encode_float(&bp,SQUELCH_CLOSE,power2dB(chan->squelch_close));
    encode_byte(&bp,THRESH_EXTEND,chan->fm.threshold);
    encode_float(&bp,PEAK_DEVIATION,chan->fm.pdeviation); // Hz
    encode_float(&bp,DEEMPH_TC,-1.0/(logf(chan->deemph.rate) * chan->output.samprate));
    encode_float(&bp,DEEMPH_GAIN,voltage2dB(chan->deemph.gain));
    break;
  case SPECT_DEMOD:
    {
      float blockrate = 1000.0f / Blocktime; // Hz
      encode_float(&bp,COHERENT_BIN_BW,blockrate);
      int N = Frontend.L + Frontend.M - 1;
      float spacing = (1 - (float)(Frontend.M-1)/N) * blockrate; // Hz
      encode_float(&bp,COHERENT_BIN_SPACING, spacing);
      encode_float(&bp,NONCOHERENT_BIN_BW,chan->spectrum.bin_bw); // Hz
      encode_int(&bp,BIN_COUNT,chan->spectrum.bin_count);
      // encode bin data here? maybe change this, it can be a lot
      // Also need to unwrap this, frequency data is dc....max positive max negative...least negative
      if(chan->spectrum.bin_data != NULL){
	// Average and clear
	float averages[chan->spectrum.bin_count];
	for(int i=0; i < chan->spectrum.bin_count; i++)
	  averages[i] = chan->spectrum.bin_data[i] / chan->blocks_since_poll;

	encode_vector(&bp,BIN_DATA,averages,chan->spectrum.bin_count);
	memset(chan->spectrum.bin_data,0,chan->spectrum.bin_count * sizeof(*chan->spectrum.bin_data));
      }
    }
    break;
  }
  // Lots of stuff not relevant in spectrum analysis mode
  if(chan->demod_type != SPECT_DEMOD){
    encode_float(&bp,LOW_EDGE,chan->filter.min_IF); // Hz
    encode_float(&bp,HIGH_EDGE,chan->filter.max_IF); // Hz
    encode_int32(&bp,OUTPUT_SAMPRATE,chan->output.samprate); // Hz
    encode_int64(&bp,OUTPUT_DATA_PACKETS,chan->output.rtp.packets);
    encode_float(&bp,KAISER_BETA,chan->filter.kaiser_beta); // Dimensionless

    // BASEBAND_POWER is now the average since last poll
    if(chan->blocks_since_poll > 0){
      float bb_power = chan->sig.bb_energy / chan->blocks_since_poll;
      encode_float(&bp,BASEBAND_POWER,power2dB(bb_power));
    }
    if(chan->blocks_since_poll > 0){
      // Output levels are already normalized since they scaled by a fixed 32767 for conversion to int16_t
      float output_power = chan->output.energy / chan->blocks_since_poll;
      encode_float(&bp,OUTPUT_LEVEL,power2dB(output_power)); // power ratio -> dB
    }
    encode_int64(&bp,OUTPUT_SAMPLES,chan->output.samples);
    encode_float(&bp,HEADROOM,voltage2dB(chan->output.headroom)); // amplitude -> dB
    // Doppler info
    encode_double(&bp,DOPPLER_FREQUENCY,chan->tune.doppler); // Hz
    encode_double(&bp,DOPPLER_FREQUENCY_RATE,chan->tune.doppler_rate); // Hz
    encode_int32(&bp,OUTPUT_CHANNELS,chan->output.channels);
    if(!isnan(chan->sig.snr) && chan->sig.snr > 0)
      encode_float(&bp,DEMOD_SNR,power2dB(chan->sig.snr)); // abs ratio -> dB

    if(chan->demod_type == LINEAR_DEMOD){ // Gain not really meaningful in FM modes
      float gain;
      if(chan->blocks_since_poll > 0){
	gain = chan->output.sum_gain_sq / chan->blocks_since_poll;
	encode_float(&bp,GAIN,power2dB(gain));
      }
    }
    // Source address we're using to send data
    encode_socket(&bp,OUTPUT_DATA_SOURCE_SOCKET,&chan->output.data_source_address);
    // Where we're sending PCM output
    encode_socket(&bp,OUTPUT_DATA_DEST_SOCKET,&chan->output.data_dest_address);
    encode_int32(&bp,OUTPUT_TTL,Mcast_ttl);
    encode_int64(&bp,OUTPUT_METADATA_PACKETS,Metadata_packets);
  }
  // Don't send test points unless they're in use
  if(!isnan(chan->tp1))
    encode_float(&bp,TP1,chan->tp1);
  if(!isnan(chan->tp2))
    encode_float(&bp,TP2,chan->tp2);
  encode_int64(&bp,BLOCKS_SINCE_POLL,chan->blocks_since_poll);

  encode_eol(&bp);

  return bp - packet;
}
