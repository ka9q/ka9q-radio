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
static unsigned long encode_radio_status(struct frontend const *frontend,struct channel *chan,uint8_t *packet, unsigned long len);

// Radio status reception and transmission thread
void *radio_status(void *arg){
  pthread_setname("radio stat");
  (void)arg; // unused

  while(true){
    // Command from user
    uint8_t buffer[PKTSIZE];
    ssize_t const length = recv(Ctl_fd,buffer,sizeof(buffer),0);
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
	    chan->status.global_timer = 0; // Just sent one
	    start_demod(chan);
	    if(Verbose)
	      fprintf(stderr,"%u dynamically started\n",ssrc); // chan->name not set yet
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
  unsigned long const len = encode_radio_status(frontend,chan,packet,sizeof(packet));
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

// Return TRUE if a restart is needed, false otherwise
bool decode_radio_commands(struct channel *chan,uint8_t const *buffer,unsigned long length){
  bool restart_needed = false;
  bool new_filter_needed = false;

  if(chan->lifetime != 0)
    chan->lifetime = Channel_idle_timeout; // restart self-destruct timer
  chan->status.packets_in++;

  // First pass to execute any PRESET command first
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

    assert(type != EOL); // Should be caught above
    switch(type){
    case PRESET: // This should be processed before any other options, regardless of order in packet
      {
	struct channel old = *chan; // Copy old to detect changes
	char *p = decode_string(cp,optlen);
	strlcpy(chan->preset,p,sizeof(chan->preset));
	FREE(p); // decode_string now allocs memory
	//	  flush_output(chan,false,true); // Flush to Ethernet before we change this
	if(Verbose > 1)
	  fprintf(stderr,"%s loadpreset(%s)\n",chan->name,chan->preset);
	if(loadpreset(chan,Preset_table,chan->preset) != 0){
	  if(Verbose)
	    fprintf(stderr,"%s loadpreset(%s) failed!\n",chan->name,chan->preset);
	  break;
	}
	if(chan->filter.min_IF > chan->filter.max_IF){
	  // Swap to ensure min <= max
	  double const tmp = chan->filter.min_IF;
	  chan->filter.min_IF = chan->filter.max_IF;
	  chan->filter.max_IF = tmp;
	}
	if(old.tune.shift != chan->tune.shift)
	  set_freq(chan,chan->tune.freq + chan->tune.shift - old.tune.shift);
	if(chan->filter.min_IF != old.filter.min_IF || chan->filter.max_IF != old.filter.max_IF || chan->filter.kaiser_beta != old.filter.kaiser_beta)
	  new_filter_needed = true;

	if(chan->demod_type != old.demod_type){
	  if(Verbose > 1)
	    fprintf(stderr,"%s demod change %s (%u) -> %s (%u)\n",chan->name,
		  demod_name_from_type(old.demod_type),old.demod_type,demod_name_from_type(chan->demod_type),chan->demod_type);

	  restart_needed = true; // chan changed, ask for a restart
	} if(chan->output.samprate != old.output.samprate){
	  if(Verbose > 1)
	    fprintf(stderr,"%s samp rate change %'u -> %'u\n",chan->name,old.output.samprate,chan->output.samprate);
	  restart_needed = true; // chan changed, ask for a restart
	}
      }
      break;
    default:
      break; // Skip other commands until second pass
    }
    cp += optlen;
  }
  // Second pass
  cp = buffer;
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

    assert(type != EOL);
    switch(type){
    case COMMAND_TAG:
      chan->status.tag = decode_int64(cp,optlen);
      break;
    case OUTPUT_SAMPRATE:
      // Restart the demodulator to recalculate filters, etc
      {
	int const new_sample_rate = round_samprate(decode_int(cp,optlen)); // Force to multiple of block rate
	if(new_sample_rate == 0)
	  break;
	// If using Opus, ignore unsupported sample rates
	if(new_sample_rate == chan->output.samprate)
	  break;
	if(chan->output.encoding == OPUS && !legal_opus_samprate(new_sample_rate))
	  break; // ignore illegal Opus sample rates (eventually will use sample rate converter)
	int pt = pt_from_info(new_sample_rate,chan->output.channels, chan->output.encoding);
	if(pt == -1){
	  fprintf(stderr,"%s can't allocate payload type for samprate %'u, channels %u, encoding %u\n",
		  chan->name,chan->output.samprate,chan->output.channels,chan->output.encoding);
	  break; // refuse to change
	}
	chan->output.rtp.type = pt;
	flush_output(chan,false,true); // Flush to Ethernet before we change this
	if(Verbose)
	  fprintf(stderr,"%s change samprate %'u -> %'u\n",chan->name,chan->output.samprate,new_sample_rate);

	chan->output.samprate = new_sample_rate;
	restart_needed = true;
      }
      break;
    case RADIO_FREQUENCY: // Hz
      {
	double const f = fabs(decode_double(cp,optlen));
	if(!isfinite(f))
	  break;

	if(Verbose > 1 && f != chan->tune.freq)
	  fprintf(stderr,"%s change freq = %'.3lf Hz\n",chan->name,f);

	set_freq(chan,f); // still call even if freq hasn't changed, to possibly reassert front end tuner control
      }
      break;
    case FIRST_LO_FREQUENCY:
      {
	double const f = decode_double(cp,optlen);
	if(!isfinite(f) || f == 0)
	  break;
	set_first_LO(chan,fabs(f)); // Will ignore it if there's no change
      }
      break;
    case SHIFT_FREQUENCY: // Hz
      {
	double const f = decode_double(cp,optlen);
	if(!isfinite(f))
	  break;
	chan->tune.shift = f;
      }
      break;
    case DOPPLER_FREQUENCY: // Hz
      {
	double const f = decode_double(cp,optlen);
	if(!isfinite(f))
	  break;
	chan->tune.doppler = f;
      }
      break;
    case DOPPLER_FREQUENCY_RATE: // Hz
      {
	double const f = decode_double(cp,optlen);
	if(!isfinite(f))
	  break;
	chan->tune.doppler_rate = f;
      }
      break;
    case LOW_EDGE: // Hz
      {
	double const f = decode_float(cp,optlen);
	if(!isfinite(f) || f == chan->filter.min_IF || f > chan->filter.max_IF)
	  break;
	chan->filter.min_IF = max(f,-(double)chan->output.samprate/2);
	new_filter_needed = true;
      }
      break;
    case HIGH_EDGE: // Hz
      {
	double const f = decode_float(cp,optlen);
	if(!isfinite(f) || f == chan->filter.max_IF || f < chan->filter.min_IF)
	  break;
	chan->filter.max_IF = min(f,(double)chan->output.samprate/2);
	new_filter_needed = true;
      }
      break;
    case KAISER_BETA: // dimensionless, always 0 or positive
        {
	  double const f = fabs(decode_float(cp,optlen));
	  if(!isfinite(f) || chan->filter.kaiser_beta == f)
	    break;
	  chan->filter.kaiser_beta = f;
	  new_filter_needed = true;
	}
      break;
    case FILTER2_KAISER_BETA: // dimensionless, always 0 or positive
        {
	  double const f = fabs(decode_float(cp,optlen));
	  if(!isfinite(f) || chan->filter2.kaiser_beta == f)
	    break;
	  chan->filter2.kaiser_beta = f;
	  new_filter_needed = true;
	}
      break;
    case DEMOD_TYPE:
      {
	enum demod_type const i = decode_int(cp,optlen);
	if(i < 0 || i >= N_DEMOD || i == chan->demod_type)
	  break;
	if(Verbose > 1)
	  fprintf(stderr,"%s demod change %s (%u) -> %s (%u)\n",chan->name,
		  demod_name_from_type(chan->demod_type),chan->demod_type,demod_name_from_type(i),i);
	chan->demod_type = i;
	restart_needed = true;
      }
      break;
    case INDEPENDENT_SIDEBAND:
      {
	bool i = decode_bool(cp,optlen); // will reimplement someday
	if(i == chan->filter2.isb)
	  break;
	chan->filter2.isb = i;
	new_filter_needed = true;
      }
      break;
    case THRESH_EXTEND:
      chan->fm.threshold = decode_bool(cp,optlen);
      break;
    case HEADROOM: // dB -> voltage, always negative dB
      {
	double const f = decode_float(cp,optlen);
	if(!isfinite(f))
	  break;
	chan->output.headroom = dB2voltage(-fabs(f));
      }
      break;
    case AGC_ENABLE:
      chan->linear.agc = decode_bool(cp,optlen);
      break;
    case GAIN:
      {
	double const f = decode_float(cp,optlen); // can be -, 0, +
	if(!isfinite(f))
	  break;
	chan->output.gain = dB2voltage(f); // -Inf = 0 gain is OK
	chan->linear.agc = false; // Doesn't make sense to change gain and then have the AGC change it again
      }
      break;
    case AGC_HANGTIME: // seconds
      {
	double const f = decode_float(cp,optlen);
	if(!isfinite(f))
	  break;
	chan->linear.hangtime = fabs(f);
      }
      break;
    case AGC_RECOVERY_RATE: // dB/sec -> amplitude / block times, always positive
      {
	double const f = decode_float(cp,optlen);
	if(!isfinite(f))
	  break;
	chan->linear.recovery_rate = dB2voltage(fabs(f));
      }
      break;
    case AGC_THRESHOLD: // dB -> amplitude
      {
	double const f = decode_float(cp,optlen);
	if(!isfinite(f))
	  break;
	chan->linear.threshold = dB2voltage(-fabs(f));
      }
      break;
    case PLL_ENABLE:
      chan->pll.enable = decode_bool(cp,optlen);
      break;
    case PLL_BW:
      {
	double const f = decode_float(cp,optlen); // Always 0 or positive
	if(!isfinite(f))
	  break;
	chan->pll.loop_bw = fabs(f);
      }
      break;
    case PLL_SQUARE:
      chan->pll.square = decode_bool(cp,optlen);
      break;
    case ENVELOPE:
      chan->linear.env = decode_bool(cp,optlen);
      break;
    case SNR_SQUELCH:
      chan->squelch.snr_enable = decode_bool(cp,optlen);
      break;
    case OUTPUT_CHANNELS: // int
      {
	int const i = decode_int(cp,optlen);
	if(i != 1 && i != 2)
	  break; // invalid

	if(chan->demod_type == WFM_DEMOD){
	  // Requesting 2 channels enables FM stereo; requesting 1 disables FM stereo
	  chan->fm.stereo_enable = (i == 2); // note boolean assignment
	} else if(i == chan->output.channels)
	  break;
	int pt = pt_from_info(chan->output.samprate,i, chan->output.encoding);
	if(pt == -1){
	  fprintf(stderr,"%s can't allocate payload type for samprate %'u, channels %u, encoding %u\n",
		  chan->name,chan->output.samprate,chan->output.channels,chan->output.encoding); // make sure it's initialized
	  break; // ignore the request
	}
	chan->output.rtp.type = pt;
	flush_output(chan,false,true); // Flush to Ethernet before we change this
	chan->output.channels = i;
      }
      break;
    case SQUELCH_OPEN:
      {
	double const x = decode_float(cp,optlen);
	if(!isfinite(x))
	  break;
	chan->squelch.open = dB2power(x);
      }
      break;
    case SQUELCH_CLOSE:
      {
        double const x = decode_float(cp,optlen);
	if(!isfinite(x))
	  break;
	chan->squelch.close = dB2power(x);
      }
      break;
    case RESOLUTION_BW:
      {
	double const x = fabs(decode_float(cp,optlen));
	if(!isfinite(x) || x == chan->spectrum.rbw)
	  break;
	if(Verbose > 1)
	  fprintf(stderr,"%s bin bw %'.1lf -> %'.1f Hz\n",chan->name,chan->spectrum.rbw,x);
	chan->spectrum.rbw = x;
      }
      break;
    case BIN_COUNT:
      {
	int const x = abs(decode_int(cp,optlen));
	if(x <= 0 || x == chan->spectrum.bin_count)
	  break;
	if(Verbose > 1)
	  fprintf(stderr,"%s bin count %u -> %u\n",chan->name,chan->spectrum.bin_count,x);
	chan->spectrum.bin_count = x;
      }
      break;
    case CROSSOVER:
      {
	double const x = fabs(decode_float(cp,optlen));
	if(!isfinite(x) || x == chan->spectrum.crossover)
	  break;
	chan->spectrum.crossover = x;
      }
      break;
    case WINDOW_TYPE:
      {
	enum window_type const i = decode_int(cp,optlen);
	if(i < 0 || i >=  N_WINDOW)
	  break;
	chan->spectrum.window_type = i;
      }
      break;
    case SPECTRUM_SHAPE: // Kaiser or gaussian
      {
	double const x = fabs(decode_float(cp,optlen)); // always positive
	if(!isfinite(x) || x == chan->spectrum.shape)
	  break;
	chan->spectrum.shape = x;
      }
      break;
    case SPECTRUM_AVG:
      {
	int x = abs(decode_int(cp,optlen));
	if(x <= 0)
	  x = 1; // Minimum 1
	if(x == chan->spectrum.fft_avg)
	  break;
	chan->spectrum.fft_avg = x;
      }
      break;
    case SPECTRUM_BASE:
      {
	double x = decode_float(cp,optlen);
	if(isfinite(x))
	  chan->spectrum.base = x;
      }
      break;
    case SPECTRUM_STEP:
      {
	double x = decode_float(cp,optlen);
	if(isfinite(x))
	  chan->spectrum.step = x;
      }
      break;
    case SPECTRUM_OVERLAP:
      {
	double x = decode_float(cp, optlen);
        if (x < 0 || x > 1)
          break;
	chan->spectrum.overlap = x;
      }
      break;
    case STATUS_INTERVAL:
      chan->status.output_interval = abs(decode_int(cp,optlen));
      break;
    case OUTPUT_ENCODING:
      {
	enum encoding encoding = decode_int(cp,optlen);
	if(encoding == chan->output.encoding || encoding < 0 || encoding >= UNUSED_ENCODING || encoding == AX25)
	  break;

	// Opus can handle only a certain set of sample rates, and it operates at 48K internally
	int samprate = chan->output.samprate;
	if(encoding == OPUS && !legal_opus_samprate(samprate))
	    samprate = OPUS_SAMPRATE; // force sample rate to 48K for Opus

	int pt = pt_from_info(samprate,chan->output.channels,encoding);
	if(pt == -1){
	  fprintf(stderr,"%s can't allocate payload type for samprate %'u, channels %u, encoding %u\n",
		  chan->name,samprate,chan->output.channels,encoding); // make sure it's initialized
	  break; // Simply refuse to change
	}
	chan->output.rtp.type = pt;
	chan->output.encoding = encoding;
	if(samprate != chan->output.samprate){
	  // Sample rate changed for Opus
	  chan->output.samprate = samprate;
	  restart_needed = true;
	}
      }
      break;
    case OPUS_BIT_RATE:
      chan->opus.bitrate = abs(decode_int(cp,optlen));
      break;
    case OPUS_DTX:
      chan->opus.dtx = decode_bool(cp,optlen);
      break;
    case OPUS_APPLICATION:
      {
	int x = decode_int(cp,optlen);
	if(x == chan->opus.application)
	  break; // no change

	for(int i=0; Opus_application[i].value != -1; i++){
	  if(Opus_application[i].value == x){
	    // Apparently this requires an encoder restart; the opus_encoder_ctl() seems to fail
	    chan->opus.application = x;
	    opus_encoder_destroy(chan->opus.encoder);
	    chan->opus.encoder = NULL;
	    break;
	  }
	}
      }
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
	double x = decode_float(cp,optlen);
	if(!isfinite(x) || chan->frontend->atten == NULL)
	  break;
	(*chan->frontend->atten)(chan->frontend,x);
      }
      break;
    case RF_GAIN:
      {
	double x = decode_float(cp,optlen);
	if(!isfinite(x) || chan->frontend->gain == NULL)
	  break;
	(*chan->frontend->gain)(chan->frontend,x);
      }
      break;
    case MINPACKET:
      {
	int i = abs(decode_int(cp,optlen));
	if(i > 6 || i == chan->output.minpacket)
	  break;
	chan->output.minpacket = i;
      }
      break;
    case FILTER2:
      {
	int i = abs(decode_int(cp,optlen));
	if(i >10 || i < 0 || i == chan->filter2.blocking)
	  break;
	chan->filter2.blocking = i;
	new_filter_needed = true;
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
  if(chan->demod_type == SPECT_DEMOD || chan->demod_type == SPECT2_DEMOD)
    memset(chan->preset,0,sizeof(chan->preset)); // No presets in this mode

  if(restart_needed){
    if(Verbose > 1)
      fprintf(stderr,"%s restarting\n",chan->name);
    return true; // A new filter will also be needed but the demod will set that up
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
static unsigned long encode_radio_status(struct frontend const *frontend,struct channel *chan,uint8_t *packet, unsigned long len){
  memset(packet,0,len);
  uint8_t *bp = packet;

  *bp++ = STATUS; // 0 = status, 1 = command

  // parameters valid in all modes
  encode_int32(&bp,OUTPUT_SSRC,chan->output.rtp.ssrc); // Now used as channel ID, so present in all modes
  encode_int64(&bp,COMMAND_TAG,chan->status.tag); // at top to make it easier to spot in dumps
  encode_int64(&bp,CMD_CNT,chan->status.packets_in); // integer
  if(strlen(frontend->description) > 0)
    encode_string(&bp,DESCRIPTION,frontend->description,strlen(frontend->description));

  encode_socket(&bp,STATUS_DEST_SOCKET,&chan->frontend->metadata_dest_socket);
  int64_t now = gps_time_ns();
  encode_int64(&bp,GPS_TIME,now);
  encode_int64(&bp,INPUT_SAMPLES,chan->filter.out.sample_index);
  encode_int32(&bp,INPUT_SAMPRATE,(uint32_t)llrint(frontend->samprate)); // Already defined on the wire as integer Hz, shouldn't change now
  encode_bool(&bp,FE_ISREAL,frontend->isreal);
  encode_double(&bp,CALIBRATE,frontend->calibrate);
  if(isfinite(frontend->rf_gain))
    encode_float(&bp,RF_GAIN,frontend->rf_gain);
  if(isfinite(frontend->rf_atten))
    encode_float(&bp,RF_ATTEN,frontend->rf_atten);
  if(isfinite(frontend->rf_level_cal))
    encode_float(&bp,RF_LEVEL_CAL,frontend->rf_level_cal); // not sent unless set
  encode_bool(&bp,RF_AGC,frontend->rf_agc);
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
  if(frontend->overranges != 0)
    encode_int64(&bp,SAMPLES_SINCE_OVER,frontend->samp_since_over);
  if(isfinite(chan->sig.n0) && chan->sig.n0 > 0)
    encode_float(&bp,NOISE_DENSITY,power2dB(chan->sig.n0));

  // Modulation mode
  encode_byte(&bp,DEMOD_TYPE,(uint8_t)chan->demod_type); // must not exceed 255 entries (unlikely)
  {
    size_t len = strlen(chan->preset);
    if(len > 0 && len < sizeof(chan->preset))
      encode_string(&bp,PRESET,chan->preset,len);
  }
  encode_float(&bp,KAISER_BETA,chan->filter.kaiser_beta); // Dimensionless

  // Mode-specific params
  switch(chan->demod_type){
  case LINEAR_DEMOD:
    encode_bool(&bp,SNR_SQUELCH,chan->squelch.snr_enable);
    encode_bool(&bp,PLL_ENABLE,chan->pll.enable); // bool
    if(chan->pll.enable){
      encode_float(&bp,FREQ_OFFSET,chan->sig.foffset);     // Hz; used differently in linear and fm
      encode_bool(&bp,PLL_LOCK,chan->pll.lock);
      encode_bool(&bp,PLL_SQUARE,chan->pll.square);
      encode_float(&bp,PLL_PHASE,chan->pll.cphase); // radians
      encode_float(&bp,PLL_BW,chan->pll.loop_bw);   // hz
      encode_int64(&bp,PLL_WRAPS,chan->pll.rotations); // count of complete 360-deg rotations of PLL phase - SIGNED
      encode_float(&bp,PLL_SNR,power2dB(chan->pll.snr)); // abs ratio -> dB
    }
    encode_float(&bp,SQUELCH_OPEN,power2dB(chan->squelch.open));
    encode_float(&bp,SQUELCH_CLOSE,power2dB(chan->squelch.close));
    encode_bool(&bp,ENVELOPE,chan->linear.env);
    encode_double(&bp,SHIFT_FREQUENCY,chan->tune.shift); // Hz
    encode_bool(&bp,AGC_ENABLE,chan->linear.agc);
    if(chan->linear.agc){
      encode_float(&bp,AGC_HANGTIME,chan->linear.hangtime); // sec
      encode_float(&bp,AGC_THRESHOLD,voltage2dB(chan->linear.threshold)); // amplitude -> dB
      encode_float(&bp,AGC_RECOVERY_RATE,voltage2dB(chan->linear.recovery_rate)); // amplitude/ -> dB/sec
    }
    encode_bool(&bp,INDEPENDENT_SIDEBAND,chan->filter2.isb);
    break;
  case FM_DEMOD:
    encode_bool(&bp,SNR_SQUELCH,chan->squelch.snr_enable);
    if(chan->fm.tone_freq != 0){
      encode_float(&bp,PL_TONE,chan->fm.tone_freq);
      encode_float(&bp,PL_DEVIATION,chan->fm.tone_deviation);
    }
    encode_float(&bp,FREQ_OFFSET,chan->sig.foffset);     // Hz; used differently in linear and fm
    encode_float(&bp,SQUELCH_OPEN,power2dB(chan->squelch.open));
    encode_float(&bp,SQUELCH_CLOSE,power2dB(chan->squelch.close));
    encode_bool(&bp,THRESH_EXTEND,chan->fm.threshold);
    encode_float(&bp,PEAK_DEVIATION,chan->fm.pdeviation); // Hz
    encode_float(&bp,DEEMPH_TC,-1.0/(log1p(-chan->fm.rate) * chan->output.samprate)); // ad-hoc
    encode_float(&bp,DEEMPH_GAIN,voltage2dB(chan->fm.gain));
    encode_float(&bp,FM_SNR,power2dB(chan->fm.snr));
    encode_bool(&bp,PLL_ENABLE,chan->pll.enable); // bool
    break;
  case WFM_DEMOD:
    // Relevant only when squelches are active
    encode_bool(&bp,SNR_SQUELCH,chan->squelch.snr_enable);
    encode_float(&bp,FREQ_OFFSET,chan->sig.foffset);     // Hz; used differently in linear and fm
    encode_float(&bp,SQUELCH_OPEN,power2dB(chan->squelch.open));
    encode_float(&bp,SQUELCH_CLOSE,power2dB(chan->squelch.close));
    encode_bool(&bp,THRESH_EXTEND,chan->fm.threshold);
    encode_float(&bp,PEAK_DEVIATION,chan->fm.pdeviation); // Hz
    encode_float(&bp,DEEMPH_TC,-1.0/(log1p(-chan->fm.rate) * FULL_SAMPRATE)); // ad-hoc
    encode_float(&bp,DEEMPH_GAIN,voltage2dB(chan->fm.gain));
    encode_float(&bp,FM_SNR,power2dB(chan->fm.snr));
    break;
  case SPECT_DEMOD:
    encode_int(&bp,WINDOW_TYPE,chan->spectrum.window_type);
    encode_float(&bp,RESOLUTION_BW,chan->spectrum.rbw); // Hz
    encode_int(&bp,BIN_COUNT,chan->spectrum.bin_count);
    encode_float(&bp,CROSSOVER,chan->spectrum.crossover);
    encode_float(&bp,SPECTRUM_SHAPE,chan->spectrum.shape);
    encode_int(&bp,SPECTRUM_FFT_N,chan->spectrum.fft_n);
    encode_float(&bp,NOISE_BW,chan->spectrum.noise_bw);
    encode_int(&bp,SPECTRUM_AVG,chan->spectrum.fft_avg);
    encode_float(&bp, SPECTRUM_OVERLAP, chan->spectrum.overlap);
    // encode bin data here? maybe change this, it can be a lot
    // Also need to unwrap this, frequency data is dc....max positive max negative...least negative
    if(chan->spectrum.bin_data != NULL){
      encode_vector(&bp,BIN_DATA,chan->spectrum.bin_data,chan->spectrum.bin_count);
    }
    break;
  case SPECT2_DEMOD:
    encode_int(&bp,WINDOW_TYPE,chan->spectrum.window_type);
    encode_float(&bp,RESOLUTION_BW,chan->spectrum.rbw); // Hz
    encode_int(&bp,BIN_COUNT,chan->spectrum.bin_count);
    encode_float(&bp,CROSSOVER,chan->spectrum.crossover);
    encode_float(&bp,SPECTRUM_SHAPE,chan->spectrum.shape);
    encode_int(&bp,SPECTRUM_FFT_N,chan->spectrum.fft_n);
    encode_float(&bp,NOISE_BW,chan->spectrum.noise_bw);
    encode_int(&bp,SPECTRUM_AVG,chan->spectrum.fft_avg);
    encode_float(&bp,SPECTRUM_BASE,chan->spectrum.base);
    encode_float(&bp,SPECTRUM_STEP,chan->spectrum.step);
    encode_float(&bp, SPECTRUM_OVERLAP, chan->spectrum.overlap);
    if(chan->spectrum.bin_data != NULL){
      uint8_t *bins = malloc(chan->spectrum.bin_count);
      if(bins == NULL){
	fprintf(stderr,"malloc of spectrum data failed\n");
      } else {
	encode_byte_data(chan,bins,chan->spectrum.base,chan->spectrum.step);
	encode_string(&bp,BIN_BYTE_DATA,bins,chan->spectrum.bin_count);
	free(bins);
      }
    }
    break;
  default:
    break;
  }

  encode_float(&bp,LOW_EDGE,chan->filter.min_IF); // Hz
  encode_float(&bp,HIGH_EDGE,chan->filter.max_IF); // Hz
  encode_int32(&bp,OUTPUT_SAMPRATE,chan->output.samprate); // Hz
  encode_float(&bp,BASEBAND_POWER,power2dB(chan->sig.bb_power));
  encode_int32(&bp,OUTPUT_CHANNELS,chan->output.channels);

  // Stuff not relevant in spectrum analysis mode
  if(chan->demod_type != SPECT_DEMOD && chan->demod_type != SPECT2_DEMOD){
    encode_int32(&bp,RTP_TIMESNAP,chan->output.rtp.timestamp);
    encode_int64(&bp,OUTPUT_DATA_PACKETS,chan->output.rtp.packets);
    encode_int(&bp,FILTER2,chan->filter2.blocking);
    if(chan->filter2.blocking != 0){
      encode_int(&bp,FILTER2_BLOCKSIZE,chan->filter2.in.ilen);
      encode_int(&bp,FILTER2_FIR_LENGTH,chan->filter2.in.impulse_length);
      encode_float(&bp,FILTER2_KAISER_BETA,chan->filter2.kaiser_beta);
    }
    // Output levels are already normalized since they scaled by a fixed 32767 for conversion to int16_t
    encode_float(&bp,OUTPUT_LEVEL,power2dB(chan->output.power)); // power ratio -> dB
    if(chan->demod_type == LINEAR_DEMOD){ // Gain not really meaningful in FM modes
      encode_float(&bp,GAIN,voltage2dB(chan->output.gain));
    }
    encode_int64(&bp,OUTPUT_SAMPLES,chan->output.samples);
    if(chan->output.encoding == OPUS || chan->output.encoding == OPUS_VOIP){
      encode_int(&bp,OPUS_BIT_RATE,chan->opus.bitrate);
      encode_int(&bp,OPUS_BANDWIDTH,chan->opus.bandwidth);
      encode_int(&bp,OPUS_APPLICATION,chan->opus.application);
      encode_int(&bp,OPUS_FEC,chan->opus.fec);
      encode_bool(&bp,OPUS_DTX,chan->opus.dtx);
    }
    encode_float(&bp,HEADROOM,voltage2dB(chan->output.headroom)); // amplitude -> dB
    // Doppler info
    encode_double(&bp,DOPPLER_FREQUENCY,chan->tune.doppler); // Hz
    encode_double(&bp,DOPPLER_FREQUENCY_RATE,chan->tune.doppler_rate); // Hz

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
    encode_byte(&bp,RTP_PT,chan->output.rtp.type);
    encode_int32(&bp,STATUS_INTERVAL,chan->status.output_interval);
    encode_int(&bp,OUTPUT_ENCODING,chan->output.encoding);
    encode_int(&bp,MINPACKET,chan->output.minpacket);
  }
  encode_int64(&bp,OUTPUT_METADATA_PACKETS,chan->status.packets_out);
  // Don't send test points unless they're in use
  if(!isnan(chan->tp1))
    encode_float(&bp,TP1,chan->tp1);
  if(!isnan(chan->tp2))
    encode_float(&bp,TP2,chan->tp2);
  encode_int64(&bp,SETOPTS,chan->options);
  encode_int64(&bp,OUTPUT_ERRORS,chan->output.errors);
  encode_eol(&bp);

  return bp - packet;
}
