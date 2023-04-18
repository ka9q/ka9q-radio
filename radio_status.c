// $Id: radio_status.c,v 1.88 2023/02/23 23:51:40 karn Exp $

#define _GNU_SOURCE 1
#include <assert.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <limits.h>
#include <pthread.h>
#include <string.h>
#include <pthread.h>
#if defined(linux)
#include <bsd/string.h>
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

extern struct demod const *Dynamic_demod;
extern dictionary const *Modetable;


static int send_radio_status(struct frontend const *frontend,struct demod const *demod,int full);
static int decode_radio_commands(struct demod *demod,uint8_t const *buffer,int length);
static int encode_radio_status(struct frontend const *frontend,struct demod const *demod,uint8_t *packet, int len);

  
// Radio status reception and transmission thread
void *radio_status(void *arg){

  char name[100];
  snprintf(name,sizeof(name),"radio stat");
  pthread_setname(name);
  
#if 0
  {
    // We start from loadconfig() after all the slices have been started so we don't contend with it for Demod_list_mutex
    pthread_mutex_lock(&Demod_list_mutex);
    for(int i = 0; i < Demod_list_length; i++){
      if(!Demod_list[i].inuse)
	continue;
      send_radio_status(&Frontend,&Demod_list[i],1); // Send status in response	
      usleep(10000); // arbitrary interval to avoid flooding the net
    }
    pthread_mutex_unlock(&Demod_list_mutex);
  }  
#endif
  while(1){
    // Command from user
    uint8_t buffer[8192];
    int const length = recv(Ctl_fd,buffer,sizeof(buffer),0);
    if(length <= 0 || buffer[0] == 0)
      continue; // short packet, or a response; ignore

    Commands++;
    // for a specific ssrc?
    uint32_t ssrc = get_ssrc(buffer+1,length-1);
    if(ssrc != 0){
      // find specific demod instance
      struct demod *demod = NULL;
      pthread_mutex_lock(&Demod_list_mutex);
      for(int i=0; i < Demod_list_length; i++){
	if(Demod_list[i].inuse && Demod_list[i].output.rtp.ssrc == ssrc){
	  demod = &Demod_list[i];
	  break;
	}
      }
      pthread_mutex_unlock(&Demod_list_mutex);
      if(demod == NULL && Dynamic_demod != NULL){
	// SSRC specified but not found; create dynamically
	demod = alloc_demod();
	memcpy(demod,Dynamic_demod,sizeof(*demod));
	// clear dynamically created objects
	demod->demod_thread = (pthread_t)0;
	demod->filter.out = NULL;
	demod->filter.energies = NULL;
	demod->tune.freq = 0;
	demod->lifetime = 20;
	demod->output.rtp.ssrc = ssrc;

	set_freq(demod,demod->tune.freq);
	start_demod(demod);
	if(Verbose)
	  fprintf(stdout,"dynamically started ssrc %u\n",ssrc);
      }
      if(demod != NULL){
	if(demod->lifetime != 0)
	  demod->lifetime = 20; // Restart 20 second self-destruct timer
	decode_radio_commands(demod,buffer+1,length-1);
	send_radio_status(&Frontend,demod,1); // Send status in response
      }
    } else {
#if 0
      // Send status for every SSRC
      pthread_mutex_lock(&Demod_list_mutex);
      for(int i=0; i < Demod_list_length; i++){
	if(Demod_list[i].inuse){
	  send_radio_status(&Frontend,&Demod_list[i],1); // Send status in response	
	  usleep(10000); // But not too quickly
	}
      }
      pthread_mutex_unlock(&Demod_list_mutex);
#endif
    }
  }
  return NULL;
}

static int send_radio_status(struct frontend const *frontend,struct demod const *demod,int full){
  uint8_t packet[2048];

  Metadata_packets++;
  int const len = encode_radio_status(frontend,demod,packet,sizeof(packet));
  send(Status_fd,packet,len,0);

  return 0;
}


// Decode and save incoming status from either SDR front end or from radio program
// from indicates source of message: from SDR front end or from radio program
// cmd == 1 means this is a command, only allow certain items

// with SSRC selection, should scan entire command for our SSRC before we execute any of it
static int decode_radio_commands(struct demod *demod,uint8_t const *buffer,int length){
  bool restart_needed = false;
  bool new_filter_needed = false;
  
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
      Command_tag = decode_int(cp,optlen);
      break;
    case OUTPUT_SAMPRATE:
      // Restart the demodulator to recalculate filters, etc
      {
	int new_sample_rate = decode_int(cp,optlen);
	if(new_sample_rate != demod->output.samprate){
	  demod->output.samprate = new_sample_rate;
	  restart_needed = true;
	}
      }
      break;
    case RADIO_FREQUENCY: // Hz
      {
	double const f = fabs(decode_double(cp,optlen));
	if(isfinite(f)){
	  if(f != demod->tune.freq && demod->demod_type == SPECT_DEMOD)
	    restart_needed = true; // Easier than trying to handle it inline
	  
	  set_freq(demod,f);
	}
      }
      break;
    case FIRST_LO_FREQUENCY:
      // control.c also sends direct to front end; do we need to do it here?
      {
	double const f = fabs(decode_double(cp,optlen));
	if(isfinite(f) && f != 0)
	  set_first_LO(demod,f); // Will ignore it if there's no change
      }
      break;
    case SECOND_LO_FREQUENCY: // Hz
      break; // No longer settable
    case SHIFT_FREQUENCY: // Hz
      {
	double const f = decode_double(cp,optlen);
	if(isfinite(f))
	  demod->tune.shift = f;
      }
      break;
    case DOPPLER_FREQUENCY: // Hz
      {
	double const f = decode_double(cp,optlen);
	if(isfinite(f))
	  demod->tune.doppler = f;
      }
      break;
    case DOPPLER_FREQUENCY_RATE: // Hz
      {
	double const f = decode_double(cp,optlen);
	if(isfinite(f))
	  demod->tune.doppler_rate = f;
      }
      break;
    case LOW_EDGE: // Hz
      {
	float const f = decode_float(cp,optlen);
	if(isfinite(f) && f != demod->filter.min_IF){
	  demod->filter.min_IF = f;
	  new_filter_needed = 1;
	}
      }
      break;
    case HIGH_EDGE: // Hz
      {
	float const f = decode_float(cp,optlen);
	if(isfinite(f) && demod->filter.max_IF != f){
	  demod->filter.max_IF = f;
	  new_filter_needed = 1;
	}
      }
      break;
      case KAISER_BETA: // dimensionless, always 0 or positive
        {
	  float const f = fabsf(decode_float(cp,optlen));
	  if(isfinite(f) && demod->filter.kaiser_beta != f)
	    demod->filter.kaiser_beta = f;
	  new_filter_needed = 1;
	}
      break;
    case PRESET:
      {
	decode_string(cp,optlen,demod->preset,sizeof(demod->preset));
	{
	  enum demod_type const old_type = demod->demod_type;
	  int const old_samprate = demod->output.samprate;
	  float const old_low = demod->filter.min_IF;
	  float const old_high = demod->filter.max_IF;
	  float const old_kaiser = demod->filter.kaiser_beta;
	  float const old_shift = demod->tune.shift;

	  loadmode(demod,Modetable,demod->preset,1);
	  if(old_shift != demod->tune.shift)
	    set_freq(demod,demod->tune.freq + demod->tune.shift - old_shift);
	  if(demod->filter.min_IF != old_low || demod->filter.max_IF != old_high || demod->filter.kaiser_beta != old_kaiser)
	    new_filter_needed = 1;

	  if(demod->demod_type != old_type || demod->output.samprate != old_samprate)
	    restart_needed = true; // demod changed, ask for a restart
	}
      }
      break;
    case DEMOD_TYPE:
      {
	enum demod_type const i = decode_int(cp,optlen);
	if(i >= 0 && i < Ndemod && i != demod->demod_type){
	  demod->demod_type = i;
	  restart_needed = true;
	}
      }
      break;
    case INDEPENDENT_SIDEBAND: // bool
      demod->filter.isb = decode_int(cp,optlen);
      break;
    case THRESH_EXTEND: // bool
      demod->fm.threshold = decode_int(cp,optlen);
      break;
    case HEADROOM: // dB -> voltage, always negative dB
      {
	float const f = decode_float(cp,optlen);
	if(isfinite(f))
	  demod->output.headroom = dB2voltage(-fabsf(f));
      }
      break;
    case AGC_ENABLE: // bool
      demod->linear.agc = decode_int(cp,optlen);
      break;
    case GAIN:
      {
	float const f = decode_float(cp,optlen);
	if(!isnan(f))
	  demod->output.gain = dB2voltage(f); // -Inf = 0 gain is OK
      }
      break;
    case AGC_HANGTIME: // seconds -> blocktimes
      {
	float const f = decode_float(cp,optlen);
	if(isfinite(f))
	  demod->linear.hangtime = fabsf(f) / (.001 * Blocktime);
      }
      break;
    case AGC_RECOVERY_RATE: // dB/sec -> amplitude / block times, always positive
      {
	float const f = decode_float(cp,optlen);
	if(!isnan(f))
	  demod->linear.recovery_rate = dB2voltage(fabsf(f) * .001 * Blocktime);
      }
      break;
    case AGC_THRESHOLD: // dB -> amplitude
      {
	float const f = decode_float(cp,optlen);
	if(isfinite(f))
	  demod->linear.threshold = dB2voltage(-fabsf(f));
      }
      break;
    case PLL_ENABLE: // bool
      demod->linear.pll = decode_int(cp,optlen);
      break;
    case PLL_BW:
      {
	float const f = decode_float(cp,optlen); // Always 0 or positive
	if(isfinite(f))
	  demod->linear.loop_bw = fabsf(f);
      }
      break;
    case PLL_SQUARE: // bool
      demod->linear.square = decode_int(cp,optlen);
      break;
    case ENVELOPE: // bool
      demod->linear.env = decode_int(cp,optlen);
      break;
    case OUTPUT_CHANNELS: // int
      {
	int const i = decode_int(cp,optlen);
	if(i == 1 || i == 2)
	  demod->output.channels = i;
      }
      break;
    case SQUELCH_OPEN:
      {
	float const x = decode_float(cp,optlen);
	if(!isnan(x))
	  demod->squelch_open = fabsf(dB2power(x));
      }	
      break;
    case SQUELCH_CLOSE:
      {
	float const x = decode_float(cp,optlen);
	if(!isnan(x))
	   demod->squelch_close = fabsf(dB2power(x));
      }	
      break;
    case NONCOHERENT_BIN_BW:
      {
	float const x = decode_float(cp,optlen);
	if(!isnan(x) && x != demod->spectrum.bin_bw){
	  demod->spectrum.bin_bw = x;
	  restart_needed = true;
	}
      }
      break;
    case BIN_COUNT:
      {
	int const x = decode_int(cp,optlen);
	if(x > 0 && x != demod->spectrum.bin_count){
	  demod->spectrum.bin_count = x;
	  restart_needed = true;
	}
      }
      break;
    case INTEGRATE_TC:
      {
	float const x = decode_float(cp,optlen);
	if(!isnan(x))
	  demod->spectrum.integrate_tc = x; // No restart needed for this parameter
      }
      break;
    default:
      break;
    }
    cp += optlen;
  }
 done:;
  if(restart_needed){
    // Stop demod
    demod->terminate = true;
    pthread_join(demod->demod_thread,NULL);
    demod->demod_thread = (pthread_t)0;
    demod->terminate = false;
  }
  if(new_filter_needed){
    // Set up new filter with demod possibly stopped
    if(demod->filter.out){
      // start_demod already sets up a new filter
      set_filter(demod->filter.out,demod->filter.min_IF/demod->output.samprate,
		 demod->filter.max_IF/demod->output.samprate,
		 demod->filter.kaiser_beta);
    }
  }    
  if(restart_needed)
    start_demod(demod);

  return 0;
}
  
// Encode contents of frontend and demod structures as command or status packet
// packet argument must be long enough!!
// Convert values from internal to engineering units
static int encode_radio_status(struct frontend const *frontend,struct demod const *demod,uint8_t *packet, int len){
  memset(packet,0,len);
  uint8_t *bp = packet;

  *bp++ = 0; // 0 = status, 1 = command

  // parameters valid in all modes
  encode_int32(&bp,COMMAND_TAG,Command_tag); // at top to make it easier to spot in dumps
  encode_int64(&bp,CMD_CNT,Commands); // integer
  if(strlen(frontend->sdr.description) > 0)
    encode_string(&bp,DESCRIPTION,frontend->sdr.description,strlen(frontend->sdr.description));
  
  // Echo timestamp from source
  encode_int64(&bp,GPS_TIME,frontend->sdr.timestamp); // integer
  // Who's sending us I/Q data
  encode_socket(&bp,INPUT_DATA_SOURCE_SOCKET,&frontend->input.data_source_address);
  // Destination address for I/Q data
  encode_socket(&bp,INPUT_DATA_DEST_SOCKET,&frontend->input.data_dest_address);
  // Source of metadata
  encode_socket(&bp,INPUT_METADATA_SOURCE_SOCKET,&frontend->input.metadata_source_address);
  // Destination address (usually multicast) and port on which we're getting metadata
  encode_socket(&bp,INPUT_METADATA_DEST_SOCKET,&frontend->input.metadata_dest_address);
  encode_int32(&bp,INPUT_SSRC,frontend->input.rtp.ssrc);
  encode_int32(&bp,INPUT_SAMPRATE,frontend->sdr.samprate); // integer Hz
  if(frontend->in){
    encode_int32(&bp,FILTER_BLOCKSIZE,frontend->in->ilen);
    encode_int32(&bp,FILTER_FIR_LENGTH,frontend->in->impulse_length);
  }

  encode_int64(&bp,INPUT_METADATA_PACKETS,frontend->input.metadata_packets); // integer
  encode_int64(&bp,INPUT_DATA_PACKETS,frontend->input.rtp.packets);
  encode_int64(&bp,INPUT_SAMPLES,frontend->input.samples);
  encode_int64(&bp,INPUT_DROPS,frontend->input.rtp.drops);
  encode_int64(&bp,INPUT_DUPES,frontend->input.rtp.dupes);
  
  // Source address we're using to send data
  encode_socket(&bp,OUTPUT_DATA_SOURCE_SOCKET,&demod->output.data_source_address);
  // Where we're sending PCM output
  encode_socket(&bp,OUTPUT_DATA_DEST_SOCKET,&demod->output.data_dest_address);
  
  encode_int32(&bp,OUTPUT_SSRC,demod->output.rtp.ssrc);
  encode_int32(&bp,OUTPUT_TTL,Mcast_ttl);
  encode_int64(&bp,OUTPUT_METADATA_PACKETS,Metadata_packets);

  
  // Lots of stuff not relevant in spectrum analysis mode
  if(demod->demod_type != SPECT_DEMOD){
    encode_int32(&bp,OUTPUT_SAMPRATE,demod->output.samprate); // Hz
    encode_int64(&bp,OUTPUT_DATA_PACKETS,demod->output.rtp.packets);
    encode_float(&bp,KAISER_BETA,demod->filter.kaiser_beta); // Dimensionless
    encode_float(&bp,BASEBAND_POWER,power2dB(demod->sig.bb_power)); // power -> dB
    encode_float(&bp,OUTPUT_LEVEL,power2dB(demod->output.level)); // power ratio -> dB
    encode_int64(&bp,OUTPUT_SAMPLES,demod->output.samples);
    encode_float(&bp,HEADROOM,voltage2dB(demod->output.headroom)); // amplitude -> dB
    // Doppler info
    encode_double(&bp,DOPPLER_FREQUENCY,demod->tune.doppler); // Hz
    encode_double(&bp,DOPPLER_FREQUENCY_RATE,demod->tune.doppler_rate); // Hz
    encode_int32(&bp,OUTPUT_CHANNELS,demod->output.channels);
    encode_float(&bp,DEMOD_SNR,power2dB(demod->sig.snr)); // abs ratio -> dB
    encode_float(&bp,FREQ_OFFSET,demod->sig.foffset);     // Hz; used differently in linear and fm
    encode_float(&bp,GAIN,voltage2dB(demod->output.gain)); // linear amplitude -> dB; fixed in FM
    encode_float(&bp,SQUELCH_OPEN,power2dB(demod->squelch_open));
    encode_float(&bp,SQUELCH_CLOSE,power2dB(demod->squelch_close));
  }
  if(demod->filter.out != NULL)
    encode_int32(&bp,FILTER_DROPS,demod->filter.out->block_drops);  // count
  
  // Signals - these ALWAYS change
  encode_float(&bp,IF_POWER,power2dB(frontend->sdr.output_level));
  encode_float(&bp,NOISE_DENSITY,power2dB(demod->sig.n0)); // power -> dB

  // Tuning
  encode_double(&bp,RADIO_FREQUENCY,demod->tune.freq); // Hz
  encode_double(&bp,SECOND_LO_FREQUENCY,demod->tune.second_LO); // Hz
  encode_double(&bp,FIRST_LO_FREQUENCY,frontend->sdr.frequency); // Hz
  encode_float(&bp,LOW_EDGE,demod->filter.min_IF); // Hz
  encode_float(&bp,HIGH_EDGE,demod->filter.max_IF); // Hz

  // Demodulation mode
  encode_byte(&bp,DEMOD_TYPE,demod->demod_type);
  {
    int len = strlen(demod->preset);
    if(len > 0 && len < sizeof(demod->preset))
      encode_string(&bp,PRESET,demod->preset,len);
  }
  // Mode-specific params
  switch(demod->demod_type){
  case LINEAR_DEMOD:
    encode_byte(&bp,PLL_ENABLE,demod->linear.pll); // bool
    if(demod->linear.pll){
      encode_byte(&bp,PLL_LOCK,demod->linear.pll_lock); // bool
      encode_byte(&bp,PLL_SQUARE,demod->linear.square); //bool
      encode_float(&bp,PLL_PHASE,demod->linear.cphase); // radians
      encode_float(&bp,PLL_BW,demod->linear.loop_bw);   // hz
    }
    encode_byte(&bp,ENVELOPE,demod->linear.env); // bool
    encode_double(&bp,SHIFT_FREQUENCY,demod->tune.shift); // Hz
    encode_byte(&bp,AGC_ENABLE,demod->linear.agc); // bool
    if(demod->linear.agc){
      encode_float(&bp,AGC_HANGTIME,demod->linear.hangtime*(.001 * Blocktime)); // samples -> sec
      encode_float(&bp,AGC_THRESHOLD,voltage2dB(demod->linear.threshold)); // amplitude -> dB
      encode_float(&bp,AGC_RECOVERY_RATE,voltage2dB(demod->linear.recovery_rate)/(.001*Blocktime)); // amplitude/block -> dB/sec
    }
    encode_byte(&bp,INDEPENDENT_SIDEBAND,demod->filter.isb); // bool
    break;
  case FM_DEMOD:
    if(demod->fm.tone_freq != 0){
      encode_float(&bp,PL_TONE,demod->fm.tone_freq);
      encode_float(&bp,PL_DEVIATION,demod->fm.tone_deviation);
    }
  case WFM_DEMOD:  // Note fall-through from FM_DEMOD
    encode_byte(&bp,THRESH_EXTEND,demod->fm.threshold);
    encode_float(&bp,PEAK_DEVIATION,demod->fm.pdeviation); // Hz
    encode_float(&bp,DEEMPH_TC,-1.0/(logf(demod->deemph.rate) * demod->output.samprate));
    encode_float(&bp,DEEMPH_GAIN,voltage2dB(demod->deemph.gain));
    break;
  case SPECT_DEMOD:
    {
      float blockrate = 1000.0f / Blocktime; // Hz
      encode_float(&bp,COHERENT_BIN_BW,blockrate);
      int N = Frontend.L + Frontend.M - 1;
      float spacing = (1 - (float)(Frontend.M-1)/N) * blockrate; // Hz
      encode_float(&bp,COHERENT_BIN_SPACING, spacing);
      encode_float(&bp,NONCOHERENT_BIN_BW,demod->spectrum.bin_bw); // Hz
      encode_int(&bp,BIN_COUNT,demod->spectrum.bin_count);
      encode_float(&bp,INTEGRATE_TC,demod->spectrum.integrate_tc); // sec
      // encode bin data here? maybe change this, it can be a lot
      // Also need to unwrap this, frequency data is dc....max positive max negative...least negative
      if(demod->spectrum.bin_data != NULL)
	encode_vector(&bp,BIN_DATA,demod->spectrum.bin_data,demod->spectrum.bin_count);
    }
    break;
  }
  // Don't send test points unless they're in use
  if(!isnan(demod->tp1))
    encode_float(&bp,TP1,demod->tp1);
  if(!isnan(demod->tp2))
    encode_float(&bp,TP2,demod->tp2);
  encode_eol(&bp);

  return bp - packet;
}
