// Generate UDP status messages from radiod, accept incoming commands to radiod in same format
// Copyright 2023-2025 Phil Karn, KA9Q

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

extern dictionary const *Preset_table;
static int encode_radio_status(struct frontend const *frontend,struct channel *chan,uint8_t *packet, int len);

// Radio status reception and transmission thread
void *radio_status(void *arg){
  pthread_setname("radio stat");
  (void)arg; // unused

  while(true){
    // Command from user
    uint8_t buffer[PKTSIZE];
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
      // Ask all threads to dump their status in a staggered manner
      for(int i=0; i < Nchannels; i++){
	struct channel *chan = &Channel_list[i];
	pthread_mutex_lock(&chan->status.lock);
	if(chan->inuse && chan->output.rtp.ssrc != 0xffffffff && chan->output.rtp.ssrc != 0)
	  chan->status.global_timer = (i >> 1) + 1; // two at a time
	pthread_mutex_unlock(&chan->status.lock);
      }
      break;
    default:
      {
	// find specific chan instance
	struct channel *chan = lookup_chan(ssrc);
	if(chan != NULL){
	  // Channel already exists; queue the command for it to execute
	  uint8_t *cmd = malloc(length-1);
	  assert(cmd != NULL);
	  memcpy(cmd,buffer+1,length-1);
	  pthread_mutex_lock(&chan->status.lock);
	  bool oops = false;
	  if(chan->status.command){
	    // An entry already exists. Drop ours, until we make this a queue
	    oops = true;
	  } else {
	    chan->status.command = cmd;
	    chan->status.length = length-1;
	  }
	  pthread_mutex_unlock(&chan->status.lock);
	  if(oops)
	    FREE(cmd);
	} else {
	  // Channel doesn't yet exist. Create, execute the rest of this command here, and then start the new demod
	  if((chan = create_chan(ssrc)) == NULL){ // possible race here?
	    // Creation failed, e.g., no output stream
	    fprintf(stderr,"Dynamic create of ssrc %'u failed; is 'data =' set in [global]?\n",ssrc);
	  } else {
	    chan->output.rtp.type = pt_from_info(chan->output.samprate,chan->output.channels,chan->output.encoding); // make sure it's initialized
	    decode_radio_commands(chan,buffer+1,length-1);
	    send_radio_status(&chan->frontend->metadata_dest_socket,chan->frontend,chan); // Send status in response
	    reset_radio_status(chan);
	    chan->status.global_timer = 0; // Just sent one
	    start_demod(chan);
	    if(Verbose)
	      fprintf(stderr,"dynamically started ssrc %'u\n",ssrc);
	  }
	}
      }
      break;
    }
  }
  return NULL;
}

int send_radio_status(struct sockaddr const *sock,struct frontend const *frontend,struct channel *chan){
  uint8_t packet[PKTSIZE];
  chan->status.packets_out++;
  int const len = encode_radio_status(frontend,chan,packet,sizeof(packet));
  // I had been forcing metadata to the ttl != 0 socket even when ttl = 0, but this creates a potential problem when
  // 1. Multiple radiod are running on the same system;
  // 2. The same SSRC is in use by more than one radiod;
  // 3. A consumer (monitor, pcmrecord) uses the source port as part of the session identifier (monitor currently does not, pcmrecord does)
  // 4. TTL is 0, so metadata is forced over the ttl != 0 socket
  //    and stream data is sent over the ttl==0 socket
  //    Then the status/data source ports may not match and the consume may think they're separate streams
  int const out_fd = (chan->output.ttl > 0) ? Output_fd : Output_fd0;
  if(sendto(out_fd,packet,len,0,sock,sizeof(struct sockaddr)) < 0)
    chan->output.errors++;

  return 0;
}
int reset_radio_status(struct channel *chan){
  // Reset integrators
  chan->status.blocks_since_poll = 0;
#if 0
  if(chan->spectrum.bin_data != NULL && chan->spectrum.bin_count != 0)
    memset(chan->spectrum.bin_data,0,chan->spectrum.bin_count * sizeof(*chan->spectrum.bin_data));
#endif
  return 0;
}

// Return TRUE if a restart is needed, false otherwise
bool decode_radio_commands(struct channel *chan,uint8_t const *buffer,int length){
  bool restart_needed = false;
  bool new_filter_needed = false;
  uint32_t const ssrc = chan->output.rtp.ssrc;

  if(chan->lifetime != 0)
    chan->lifetime = Channel_idle_timeout; // restart self-destruct timer
  chan->status.packets_in++;

  uint8_t const *cp = buffer;

  while(cp < buffer + length){
    enum status_type const type = *cp++; // increment cp to length field

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
    if(cp + optlen >= buffer + length)
      break; // invalid length; we can't continue to scan

    switch(type){
    case EOL: // Shouldn't get here
      goto done;
    case COMMAND_TAG:
      chan->status.tag = decode_int32(cp,optlen);
      break;
    case OUTPUT_SAMPRATE:
      // Restart the demodulator to recalculate filters, etc
      {
	unsigned int const new_sample_rate = round_samprate(decode_int(cp,optlen)); // Force to multiple of block rate
	// If using Opus, ignore unsupported sample rates
	if(new_sample_rate != chan->output.samprate){
	  if(chan->output.encoding != OPUS || new_sample_rate == 48000 || new_sample_rate == 24000 || new_sample_rate == 16000 || new_sample_rate == 12000 || new_sample_rate == 8000){
	    flush_output(chan,false,true); // Flush to Ethernet before we change this
	    chan->output.samprate = new_sample_rate;
	    chan->output.rtp.type = pt_from_info(chan->output.samprate,chan->output.channels,chan->output.encoding);
	    restart_needed = true;
	  }
	}
      }
      break;
    case RADIO_FREQUENCY: // Hz
      {
	double const f = fabs(decode_double(cp,optlen));
	if(isfinite(f)){
	  if(Verbose > 1)
	    fprintf(stderr,"set ssrc %u freq = %'.3lf\n",ssrc,f);

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
	  chan->filter.min_IF = max(f,-(float)chan->output.samprate/2);
	  new_filter_needed = true;
	}
      }
      break;
    case HIGH_EDGE: // Hz
      {
	float const f = decode_float(cp,optlen);
	if(isfinite(f) && chan->filter.max_IF != f){
	  chan->filter.max_IF = min(f,(float)chan->output.samprate/2);
	  new_filter_needed = true;
	}
      }
      break;
      case KAISER_BETA: // dimensionless, always 0 or positive
        {
	  float const f = fabsf(decode_float(cp,optlen));
	  if(isfinite(f) && chan->filter.kaiser_beta != f){
	    chan->filter.kaiser_beta = f;
	    new_filter_needed = true;
	  }
	}
      break;
      case FILTER2_KAISER_BETA: // dimensionless, always 0 or positive
        {
	  float const f = fabsf(decode_float(cp,optlen));
	  if(isfinite(f) && chan->filter2.kaiser_beta != f){
	    chan->filter2.kaiser_beta = f;
	    new_filter_needed = true;
	  }
	}
      break;
    case PRESET:
      {
	char *p = decode_string(cp,optlen);
	strlcpy(chan->preset,p,sizeof(chan->preset));
	FREE(p); // decode_string now allocs memory
	{
	  flush_output(chan,false,true); // Flush to Ethernet before we change this
	  enum demod_type const old_type = chan->demod_type;
	  unsigned int const old_samprate = chan->output.samprate;
	  float const old_low = chan->filter.min_IF;
	  float const old_high = chan->filter.max_IF;
	  float const old_kaiser = chan->filter.kaiser_beta;
	  float const old_shift = chan->tune.shift;

	  if(Verbose > 1)
	    fprintf(stderr,"command loadpreset(ssrc=%u) mode=%s\n",ssrc,chan->preset);
	  if(loadpreset(chan,Preset_table,chan->preset) != 0){
	    if(Verbose)
	      fprintf(stderr,"command loadpreset(ssrc=%u) mode=%sfailed!\n",ssrc,chan->preset);
	    break;
	  }
	  if(old_shift != chan->tune.shift)
	    set_freq(chan,chan->tune.freq + chan->tune.shift - old_shift);
	  if(chan->filter.min_IF != old_low || chan->filter.max_IF != old_high || chan->filter.kaiser_beta != old_kaiser)
	    new_filter_needed = true;

	  if(chan->demod_type != old_type || chan->output.samprate != old_samprate){
	    if(Verbose > 1)
	      fprintf(stderr,"demod %d -> %d, samprate %d -> %d\n",old_type,chan->demod_type,old_samprate,chan->output.samprate);
	    restart_needed = true; // chan changed, ask for a restart
	  }
	}
      }
      break;
    case DEMOD_TYPE:
      {
	enum demod_type const i = decode_int(cp,optlen);
	if(i >= 0 && i < N_DEMOD && i != chan->demod_type){
	  if(Verbose > 1)
	    fprintf(stderr,"Demod change %d -> %d\n",chan->demod_type,i);
	  chan->demod_type = i;
	  restart_needed = true;
	}
      }
      break;
    case INDEPENDENT_SIDEBAND:
      {
	bool i = decode_bool(cp,optlen); // will reimplement someday
	if(i != chan->filter2.isb){
	  chan->filter2.isb = i;
	  new_filter_needed = true;
	}
      }
      break;
    case THRESH_EXTEND:
      chan->fm.threshold = decode_bool(cp,optlen);
      break;
    case HEADROOM: // dB -> voltage, always negative dB
      {
	float const f = decode_float(cp,optlen);
	if(isfinite(f))
	  chan->output.headroom = dB2voltage(-fabsf(f));
      }
      break;
    case AGC_ENABLE:
      chan->linear.agc = decode_bool(cp,optlen);
      break;
    case GAIN:
      {
	float const f = decode_float(cp,optlen);
	if(isfinite(f)){
	  chan->output.gain = dB2voltage(f); // -Inf = 0 gain is OK
	  chan->linear.agc = false; // Doesn't make sense to change gain and then have the AGC change it again
	}
      }
      break;
    case AGC_HANGTIME: // seconds
      {
	float const f = decode_float(cp,optlen);
	if(isfinite(f))
	  chan->linear.hangtime = fabsf(f);
      }
      break;
    case AGC_RECOVERY_RATE: // dB/sec -> amplitude / block times, always positive
      {
	float const f = decode_float(cp,optlen);
	if(isfinite(f))
	  chan->linear.recovery_rate = dB2voltage(fabsf(f));
      }
      break;
    case AGC_THRESHOLD: // dB -> amplitude
      {
	float const f = decode_float(cp,optlen);
	if(isfinite(f))
	  chan->linear.threshold = dB2voltage(-fabsf(f));
      }
      break;
    case PLL_ENABLE:
      chan->pll.enable = decode_bool(cp,optlen);
      break;
    case PLL_BW:
      {
	float const f = decode_float(cp,optlen); // Always 0 or positive
	if(isfinite(f))
	  chan->pll.loop_bw = fabsf(f);
      }
      break;
    case PLL_SQUARE:
      chan->pll.square = decode_bool(cp,optlen);
      break;
    case ENVELOPE:
      chan->linear.env = decode_bool(cp,optlen);
      break;
    case SNR_SQUELCH:
      chan->snr_squelch_enable = decode_bool(cp,optlen);
      break;
    case OUTPUT_CHANNELS: // int
      {
	unsigned int const i = decode_int(cp,optlen);
	if(i != 1 && i != 2)
	  break; // invalid

	if(chan->demod_type == WFM_DEMOD){
	  // Requesting 2 channels enables FM stereo; requesting 1 disables FM stereo
	  chan->fm.stereo_enable = (i == 2); // note boolean assignment
	} else if(i != chan->output.channels){
	    flush_output(chan,false,true); // Flush to Ethernet before we change this
	    chan->output.channels = i;
	    chan->output.rtp.type = pt_from_info(chan->output.samprate,chan->output.channels,chan->output.encoding);
	}
      }
      break;
    case SQUELCH_OPEN:
      {
	float const x = decode_float(cp,optlen);
	if(isfinite(x))
	  chan->squelch_open = fabsf(dB2power(x));
      }
      break;
    case SQUELCH_CLOSE:
      {
	float const x = decode_float(cp,optlen);
	if(isfinite(x))
	   chan->squelch_close = fabsf(dB2power(x));
      }
      break;
    case NONCOHERENT_BIN_BW:
      {
	float const x = decode_float(cp,optlen);
	if(isfinite(x) && x != chan->spectrum.bin_bw){
	  if(Verbose > 1)
	    fprintf(stderr,"bin bw %f -> %f\n",chan->spectrum.bin_bw,x);
	  chan->spectrum.bin_bw = x;
	}
      }
      break;
    case BIN_COUNT:
      {
	int const x = decode_int(cp,optlen);
	if(x > 0 && x != chan->spectrum.bin_count){
	  if(Verbose > 1)
	    fprintf(stderr,"bin count %d -> %d\n",chan->spectrum.bin_count,x);
	  chan->spectrum.bin_count = x;
	}
      }
      break;
    case CROSSOVER:
      {
	float const x = decode_float(cp,optlen);
	if(x >= 0)
	  chan->spectrum.crossover = x;
      }
      break;
    case STATUS_INTERVAL:
      {
	int const x = decode_int(cp,optlen);
	if(x >= 0)
	  chan->status.output_interval = x;
      }
      break;
    case OUTPUT_ENCODING:
      {
	enum encoding encoding = decode_int(cp,optlen);
	if(encoding != chan->output.encoding && encoding >= NO_ENCODING && encoding < UNUSED_ENCODING){
	  flush_output(chan,false,true); // Flush to Ethernet before we change this
	  chan->output.encoding = encoding;
	  // Opus can handle only a certain set of sample rates, and it operates at 48K internally
	  if(encoding == OPUS && chan->output.samprate != 48000 && chan->output.samprate != 24000
	     && chan->output.samprate != 16000 && chan->output.samprate != 12000 && chan->output.samprate != 8000){
	    chan->output.samprate = 48000;
	    restart_needed = true;
	  }
	  chan->output.rtp.type = pt_from_info(chan->output.samprate,chan->output.channels,chan->output.encoding);
	}
      }
      break;
    case OPUS_BIT_RATE:
      chan->output.opus_bitrate = abs(decode_int(cp,optlen));
      break;
    case SETOPTS:
      {
	uint64_t opts = decode_int64(cp,optlen);
	chan->options |= opts;
      }
      break;
    case CLEAROPTS:
      {
	uint64_t opts = decode_int64(cp,optlen);
	chan->options &= ~opts;
      }
      break;
    case RF_ATTEN:
      {
	float x = decode_float(cp,optlen);
	if(!isnan(x) && chan->frontend->atten != NULL)
	  (*chan->frontend->atten)(chan->frontend,x);
      }
      break;
    case RF_GAIN:
      {
	float x = decode_float(cp,optlen);
	if(!isnan(x) && chan->frontend->gain != NULL)
	  (*chan->frontend->gain)(chan->frontend,x);
      }
      break;
    case MINPACKET:
      {
	unsigned int i = decode_int(cp,optlen);
	if(i <= 4 && i != chan->output.minpacket){
	  chan->output.minpacket = i;
	}
      }
      break;
    case FILTER2:
      {
	unsigned int i = decode_int(cp,optlen);
	if(i > 10)
	  i = 10;
	if(i != chan->filter2.blocking){
	  chan->filter2.blocking = i;
	  new_filter_needed = true;
	}
      }
      break;
    case OUTPUT_DATA_DEST_SOCKET:
      {
	// Actually sets both data and status, overriding port numbers
	decode_socket(&chan->output.dest_socket,cp,optlen);
	setport(&chan->output.dest_socket,DEFAULT_RTP_PORT);
	chan->status.dest_socket = chan->output.dest_socket;
	setport(&chan->status.dest_socket,DEFAULT_STAT_PORT);
      }
      break;
    default:
      break;
    }
    cp += optlen;
  }
 done:;
  if(chan->demod_type == SPECT_DEMOD)
    memset(chan->preset,0,sizeof(chan->preset)); // No presets in this mode

  if(restart_needed){
    if(Verbose > 1)
      fprintf(stderr,"restarting thread for ssrc %u\n",ssrc);
    return true;
  }
  if(new_filter_needed){
    set_channel_filter(chan);
    // Retune if necessary to accommodate edge of passband
    // but only if a change was commanded, to prevent a tuning war
    set_freq(chan,chan->tune.freq);
    chan->filter.remainder = NAN; // Force re-init of fine oscillator
  }
  return false;
}

// Encode contents of frontend and chan structures as command or status packet
// packet argument must be long enough!!
// Convert values from internal to engineering units
static int encode_radio_status(struct frontend const *frontend,struct channel *chan,uint8_t *packet, int len){
  memset(packet,0,len);
  uint8_t *bp = packet;

  *bp++ = STATUS; // 0 = status, 1 = command

  // parameters valid in all modes
  encode_int32(&bp,OUTPUT_SSRC,chan->output.rtp.ssrc); // Now used as channel ID, so present in all modes
  encode_int32(&bp,COMMAND_TAG,chan->status.tag); // at top to make it easier to spot in dumps
  encode_int64(&bp,CMD_CNT,chan->status.packets_in); // integer
  if(strlen(frontend->description) > 0)
    encode_string(&bp,DESCRIPTION,frontend->description,strlen(frontend->description));

  // Snapshot the output RTP timestamp
  encode_int32(&bp,RTP_TIMESNAP,chan->output.rtp.timestamp);
  encode_socket(&bp,STATUS_DEST_SOCKET,&chan->frontend->metadata_dest_socket);
  int64_t now = gps_time_ns();
  encode_int64(&bp,GPS_TIME,now);
  encode_int64(&bp,INPUT_SAMPLES,frontend->samples);
  encode_int32(&bp,INPUT_SAMPRATE,frontend->samprate); // integer Hz
  encode_int32(&bp,FE_ISREAL,frontend->isreal ? true : false);
  encode_double(&bp,CALIBRATE,frontend->calibrate);
  encode_float(&bp,RF_GAIN,frontend->rf_gain);
  encode_float(&bp,RF_ATTEN,frontend->rf_atten);
  encode_float(&bp,RF_LEVEL_CAL,frontend->rf_level_cal);
  encode_int(&bp,RF_AGC,frontend->rf_agc);
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

  encode_int32(&bp,FILTER_BLOCKSIZE,frontend->in.ilen);
  encode_int32(&bp,FILTER_FIR_LENGTH,frontend->in.impulse_length);
  encode_int32(&bp,FILTER_DROPS,chan->filter.out.block_drops);  // count

  // Adjust for A/D width
  // Level is absolute relative to A/D saturation, so +3dB for real vs complex
  encode_float(&bp,IF_POWER,power2dB(frontend->if_power * scale_ADpower2FS(frontend)));
  encode_int64(&bp,AD_OVER,frontend->overranges);
  encode_int64(&bp,SAMPLES_SINCE_OVER,frontend->samp_since_over);
  encode_float(&bp,NOISE_DENSITY,power2dB(chan->sig.n0));

  // Modulation mode
  encode_byte(&bp,DEMOD_TYPE,chan->demod_type);
  {
    size_t len = strlen(chan->preset);
    if(len > 0 && len < sizeof(chan->preset))
      encode_string(&bp,PRESET,chan->preset,len);
  }


  // Mode-specific params
  switch(chan->demod_type){
  case LINEAR_DEMOD:
    encode_byte(&bp,SNR_SQUELCH,chan->snr_squelch_enable);
    encode_byte(&bp,PLL_ENABLE,chan->pll.enable); // bool
    if(chan->pll.enable){
      encode_float(&bp,FREQ_OFFSET,chan->sig.foffset);     // Hz; used differently in linear and fm
      encode_byte(&bp,PLL_LOCK,chan->pll.lock); // bool
      encode_byte(&bp,PLL_SQUARE,chan->pll.square); //bool
      encode_float(&bp,PLL_PHASE,chan->pll.cphase); // radians
      encode_float(&bp,PLL_BW,chan->pll.loop_bw);   // hz
      encode_int64(&bp,PLL_WRAPS,chan->pll.rotations); // count of complete 360-deg rotations of PLL phase
      encode_float(&bp,PLL_SNR,power2dB(chan->pll.snr)); // abs ratio -> dB
    }
    encode_float(&bp,SQUELCH_OPEN,power2dB(chan->squelch_open));
    encode_float(&bp,SQUELCH_CLOSE,power2dB(chan->squelch_close));
    encode_byte(&bp,ENVELOPE,chan->linear.env); // bool
    encode_double(&bp,SHIFT_FREQUENCY,chan->tune.shift); // Hz
    encode_byte(&bp,AGC_ENABLE,chan->linear.agc); // bool
    if(chan->linear.agc){
      encode_float(&bp,AGC_HANGTIME,chan->linear.hangtime); // sec
      encode_float(&bp,AGC_THRESHOLD,voltage2dB(chan->linear.threshold)); // amplitude -> dB
      encode_float(&bp,AGC_RECOVERY_RATE,voltage2dB(chan->linear.recovery_rate)); // amplitude/ -> dB/sec
    }
    encode_byte(&bp,INDEPENDENT_SIDEBAND,chan->filter2.isb);
    break;
  case FM_DEMOD:
    encode_byte(&bp,SNR_SQUELCH,chan->snr_squelch_enable);
    if(chan->fm.tone_freq != 0){
      encode_float(&bp,PL_TONE,chan->fm.tone_freq);
      encode_float(&bp,PL_DEVIATION,chan->fm.tone_deviation);
    }
    encode_float(&bp,FREQ_OFFSET,chan->sig.foffset);     // Hz; used differently in linear and fm
    encode_float(&bp,SQUELCH_OPEN,power2dB(chan->squelch_open));
    encode_float(&bp,SQUELCH_CLOSE,power2dB(chan->squelch_close));
    encode_byte(&bp,THRESH_EXTEND,chan->fm.threshold);
    encode_float(&bp,PEAK_DEVIATION,chan->fm.pdeviation); // Hz
    encode_float(&bp,DEEMPH_TC,-1.0f/(log1pf(-chan->fm.rate) * chan->output.samprate)); // ad-hoc
    encode_float(&bp,DEEMPH_GAIN,voltage2dB(chan->fm.gain));
    encode_float(&bp,FM_SNR,power2dB(chan->fm.snr));
    break;
  case WFM_DEMOD:
    // Relevant only when squelches are active
    encode_byte(&bp,SNR_SQUELCH,chan->snr_squelch_enable);
    encode_float(&bp,FREQ_OFFSET,chan->sig.foffset);     // Hz; used differently in linear and fm
    encode_float(&bp,SQUELCH_OPEN,power2dB(chan->squelch_open));
    encode_float(&bp,SQUELCH_CLOSE,power2dB(chan->squelch_close));
    encode_byte(&bp,THRESH_EXTEND,chan->fm.threshold);
    encode_float(&bp,PEAK_DEVIATION,chan->fm.pdeviation); // Hz
    encode_float(&bp,DEEMPH_TC,-1.0f/(log1pf(-chan->fm.rate) * 48000.0f)); // ad-hoc
    encode_float(&bp,DEEMPH_GAIN,voltage2dB(chan->fm.gain));
    encode_float(&bp,FM_SNR,power2dB(chan->fm.snr));
    break;
  case SPECT_DEMOD:
    {
      encode_float(&bp,NONCOHERENT_BIN_BW,chan->spectrum.bin_bw); // Hz
      encode_int(&bp,BIN_COUNT,chan->spectrum.bin_count);
      encode_float(&bp,CROSSOVER,chan->spectrum.crossover);
      // encode bin data here? maybe change this, it can be a lot
      // Also need to unwrap this, frequency data is dc....max positive max negative...least negative
      spectrum_poll(chan); // Update the spectral data (wide bins only)
      if(chan->spectrum.bin_data != NULL){
#if 0
	// Average and clear
	float const scale = 1.f / chan->status.blocks_since_poll;
	for(int i=0; i < chan->spectrum.bin_count; i++)
	  chan->spectrum.bin_data[i] *= scale;
#endif
	encode_vector(&bp,BIN_DATA,chan->spectrum.bin_data,chan->spectrum.bin_count);
      }
    }
    break;
  default:
    break;
  }

  encode_float(&bp,LOW_EDGE,chan->filter.min_IF); // Hz
  encode_float(&bp,HIGH_EDGE,chan->filter.max_IF); // Hz
  // Lots of stuff not relevant in spectrum analysis mode
  if(chan->demod_type != SPECT_DEMOD){
    encode_int32(&bp,OUTPUT_SAMPRATE,chan->output.samprate); // Hz
    encode_int64(&bp,OUTPUT_DATA_PACKETS,chan->output.rtp.packets);
    encode_float(&bp,KAISER_BETA,chan->filter.kaiser_beta); // Dimensionless
    encode_int(&bp,FILTER2,chan->filter2.blocking);
    if(chan->filter2.blocking != 0){
      encode_int(&bp,FILTER2_BLOCKSIZE,chan->filter2.in.ilen);
      encode_int(&bp,FILTER2_FIR_LENGTH,chan->filter2.in.impulse_length);
      encode_float(&bp,FILTER2_KAISER_BETA,chan->filter2.kaiser_beta);
    }
    encode_float(&bp,BASEBAND_POWER,power2dB(chan->sig.bb_power));
    // Output levels are already normalized since they scaled by a fixed 32767 for conversion to int16_t
    encode_float(&bp,OUTPUT_LEVEL,power2dB(chan->output.power)); // power ratio -> dB
    if(chan->demod_type == LINEAR_DEMOD){ // Gain not really meaningful in FM modes
      encode_float(&bp,GAIN,voltage2dB(chan->output.gain));
    }
    encode_int64(&bp,OUTPUT_SAMPLES,chan->output.samples);
    encode_int32(&bp,OPUS_BIT_RATE,chan->output.opus_bitrate);
    encode_float(&bp,HEADROOM,voltage2dB(chan->output.headroom)); // amplitude -> dB
    // Doppler info
    encode_double(&bp,DOPPLER_FREQUENCY,chan->tune.doppler); // Hz
    encode_double(&bp,DOPPLER_FREQUENCY_RATE,chan->tune.doppler_rate); // Hz
    encode_int32(&bp,OUTPUT_CHANNELS,chan->output.channels);

    // Source address we're using to send data
    // Get the local socket for the output stream
    // Going connectionless with Output_fd broke this. The source port is filled in, but the source address is all zeroes because
    // it depends on the specific output address, which is only known from a routing table lookup. Oh well.
    // Also this doesn't return anything until the socket is first transmitted on
    {
      socklen_t len = sizeof(chan->output.source_socket);
      int const outsock = chan->output.ttl != 0 ? Output_fd : Output_fd0;
      getsockname(outsock,(struct sockaddr *)&chan->output.source_socket,&len);
    }
    encode_socket(&bp,OUTPUT_DATA_SOURCE_SOCKET,&chan->output.source_socket);
    // Where we're sending PCM output
    encode_socket(&bp,OUTPUT_DATA_DEST_SOCKET,&chan->output.dest_socket);
    encode_int32(&bp,OUTPUT_TTL,chan->output.ttl);
    encode_int64(&bp,OUTPUT_METADATA_PACKETS,chan->status.packets_out);
    encode_byte(&bp,RTP_PT,chan->output.rtp.type);
    encode_int32(&bp,STATUS_INTERVAL,chan->status.output_interval);
    encode_int(&bp,OUTPUT_ENCODING,chan->output.encoding);
    encode_int(&bp,MINPACKET,chan->output.minpacket);
  }
  // Don't send test points unless they're in use
  if(!isnan(chan->tp1))
    encode_float(&bp,TP1,chan->tp1);
  if(!isnan(chan->tp2))
    encode_float(&bp,TP2,chan->tp2);
  encode_int64(&bp,BLOCKS_SINCE_POLL,chan->status.blocks_since_poll);
  encode_int64(&bp,SETOPTS,chan->options);
  encode_int64(&bp,OUTPUT_ERRORS,chan->output.errors);
  encode_eol(&bp);

  return bp - packet;
}
