// decode_radio_status: decode incoming status messages from radiod
// Intended for applications, should be moved into an API library and rewritten to not
// share internal data structures with radiod
// Copyright 2026 Phil Karn, KA9Q

#include <string.h>
#include "radio.h"

// Decode incoming status message from the radio program, convert and fill in fields in local channel structure
// Leave all other fields unchanged, as they may have local uses (e.g., file descriptors)
// Note that we use some fields in channel differently than in radiod (e.g., dB vs ratios)
int decode_radio_status(struct frontend *frontend,chan_t *chan,uint8_t const *buffer,int length){
  if(frontend == NULL || chan == NULL || buffer == NULL)
    return -1;
  if(length <= 0)
    return 0;
  uint8_t const *cp = buffer;
  while(cp  < &buffer[length-1]){ // ensure at least 2 bytes for type & length
    enum status_type type = *cp++; // increment to length field
    if(type == EOL)
      break; // end of list
    unsigned int optlen = *cp++;
    if(optlen & 0x80){
      // length is >= 128 bytes; fetch actual length from next N bytes, where N is low 7 bits of optlen
      int length_of_length = optlen & 0x7f;
      // Reject implausible or overflowing lengths, and ensure the length
      // bytes themselves fit in the remaining packet.
      if(length_of_length > (int)sizeof(optlen) || cp + length_of_length > &buffer[length])
	break;
      optlen = 0;
      while(length_of_length > 0){
	optlen <<= 8;
	optlen |= *cp++;
	length_of_length--;
      }
    }
    if(cp + optlen > &buffer[length])
      break; // value would run past end of packet
    switch(type){
    case EOL:
      break;
    case CMD_CNT:
      chan->status.packets_in = decode_int32(cp,optlen);
      break;
    case DESCRIPTION:
      {
	char *str = decode_string(cp,optlen);
	if(str != NULL)
	  strlcpy(frontend->description,str,sizeof(frontend->description));
	FREE(str);
      }
      break;
    case RTP_TIMESNAP:
      chan->output.time_snap = decode_int(cp,optlen);
      chan->output.rtp.timestamp = chan->output.time_snap; // is this duplicated?
      break;
    case STATUS_DEST_SOCKET:
      decode_socket(&frontend->metadata_dest_socket,cp,optlen);
      break;
    case GPS_TIME:
      chan->clocktime = decode_int64(cp,optlen);
      break;
    case INPUT_SAMPRATE:
      frontend->samprate = decode_int(cp,optlen);
      break;
    case INPUT_SAMPLES:
      frontend->samples = decode_int64(cp,optlen);
      break;
    case AD_OVER:
      frontend->overranges = decode_int64(cp,optlen);
      break;
    case SAMPLES_SINCE_OVER:
      frontend->samp_since_over = decode_int64(cp,optlen);
      break;
    case OUTPUT_DATA_SOURCE_SOCKET:
      decode_socket(&chan->output.source_socket,cp,optlen);
      break;
    case OUTPUT_DATA_DEST_SOCKET:
      decode_socket(&chan->output.dest_socket,cp,optlen);
      break;
    case OUTPUT_SSRC:
      chan->output.rtp.ssrc = decode_int32(cp,optlen);
      break;
    case OUTPUT_TTL:
      chan->output.ttl = decode_int8(cp,optlen);
      break;
    case OUTPUT_SAMPRATE:
      chan->output.samprate = decode_int(cp,optlen);
      break;
    case OUTPUT_DATA_PACKETS:
      chan->output.rtp.packets = decode_int64(cp,optlen);
      break;
    case OUTPUT_METADATA_PACKETS:
      chan->status.packets_out = decode_int64(cp,optlen);
      break;
    case FILTER_BLOCKSIZE:
      frontend->L = decode_int(cp,optlen);
      break;
    case FILTER_FIR_LENGTH:
      frontend->M = decode_int(cp,optlen);
      break;
    case LOW_EDGE:
      chan->filter.min_IF = decode_float(cp,optlen);
      break;
    case HIGH_EDGE:
      chan->filter.max_IF = decode_float(cp,optlen);
      break;
    case FE_LOW_EDGE:
      frontend->min_IF = decode_float(cp,optlen);
      break;
    case FE_HIGH_EDGE:
      frontend->max_IF = decode_float(cp,optlen);
      break;
    case FE_ISREAL:
      frontend->isreal = decode_bool(cp,optlen);
      break;
    case AD_BITS_PER_SAMPLE:
      frontend->bitspersample = decode_int(cp,optlen);
      break;
    case CALIBRATE:
      frontend->calibrate = decode_double(cp,optlen);
      break;
    case IF_GAIN:
      frontend->if_gain = decode_int8(cp,optlen);
      break;
    case LNA_GAIN:
      frontend->lna_gain = decode_int8(cp,optlen);
      break;
    case MIXER_GAIN:
      frontend->mixer_gain = decode_int8(cp,optlen);
      break;
    case KAISER_BETA:
      chan->filter.kaiser_beta = decode_float(cp,optlen);
      break;
    case FILTER_DROPS:
      chan->filter.out.block_drops = decode_int(cp,optlen);
      break;
    case IF_POWER:
      frontend->if_power = dB2power(decode_float(cp,optlen));
      break;
    case BASEBAND_POWER:
      chan->sig.bb_power = dB2power(decode_float(cp,optlen)); // dB -> power
      break;
    case NOISE_DENSITY:
      chan->sig.n0 = dB2power(decode_float(cp,optlen));
      break;
    case PLL_SNR:
      chan->pll.snr = dB2power(decode_float(cp,optlen));
      break;
    case FM_SNR:
      chan->fm.snr = dB2power(decode_float(cp,optlen));
      break;
    case FREQ_OFFSET:
      chan->sig.foffset = decode_float(cp,optlen);
      break;
    case PEAK_DEVIATION:
      chan->fm.pdeviation = decode_float(cp,optlen);
      break;
    case PLL_LOCK:
      chan->pll.lock = decode_bool(cp,optlen);
      break;
    case PLL_BW:
      chan->pll.loop_bw = decode_float(cp,optlen);
      break;
    case PLL_SQUARE:
      chan->pll.square = decode_bool(cp,optlen);
      break;
    case PLL_PHASE:
      chan->pll.cphase = decode_float(cp,optlen);
      break;
    case PLL_WRAPS:
      chan->pll.rotations = (int64_t)decode_int64(cp,optlen);
      break;
    case ENVELOPE:
      chan->linear.env = decode_bool(cp,optlen);
      break;
    case SNR_SQUELCH:
      chan->squelch.snr_enable = decode_bool(cp,optlen);
      break;
    case OUTPUT_LEVEL:
      chan->output.power = dB2power(decode_float(cp,optlen));
      break;
    case OUTPUT_SAMPLES:
      chan->output.samples = decode_int64(cp,optlen);
      break;
    case COMMAND_TAG:
      chan->status.tag = decode_int64(cp,optlen);
      break;
    case RADIO_FREQUENCY:
      chan->tune.freq = decode_double(cp,optlen);
      break;
    case SECOND_LO_FREQUENCY:
      chan->tune.second_LO = decode_double(cp,optlen);
      break;
    case SHIFT_FREQUENCY:
      chan->tune.shift = decode_double(cp,optlen);
      break;
    case FIRST_LO_FREQUENCY:
      frontend->frequency = decode_double(cp,optlen);
      break;
    case DOPPLER_FREQUENCY:
      chan->tune.doppler = decode_double(cp,optlen);
      break;
    case DOPPLER_FREQUENCY_RATE:
      chan->tune.doppler_rate = decode_double(cp,optlen);
      break;
    case DEMOD_TYPE:
      chan->demod_type = decode_int(cp,optlen);
      break;
    case OUTPUT_CHANNELS:
      chan->output.channels = decode_int(cp,optlen);
      break;
    case INDEPENDENT_SIDEBAND:
      chan->filter2.out.isb = decode_bool(cp,optlen);
      break;
    case THRESH_EXTEND:
      chan->fm.threshold = decode_bool(cp,optlen);
      break;
    case PLL_ENABLE:
      chan->pll.enable = decode_bool(cp,optlen);
      break;
    case GAIN:              // dB to voltage
      chan->output.gain = dB2voltage(decode_float(cp,optlen));
      break;
    case AGC_ENABLE:
      chan->linear.agc = decode_bool(cp,optlen);
      break;
    case HEADROOM:          // db to voltage
      chan->output.headroom = dB2voltage(decode_float(cp,optlen));
      break;
    case AGC_HANGTIME:      // s to samples
      chan->linear.hangtime = decode_float(cp,optlen);
      break;
    case AGC_RECOVERY_RATE: // dB/s to dB/sample to voltage/sample
      chan->linear.recovery_rate = dB2voltage(decode_float(cp,optlen));
      break;
    case AGC_THRESHOLD:   // dB to voltage
      chan->linear.threshold = dB2voltage(decode_float(cp,optlen));
      break;
    case TP1: // Test point
      chan->tp1 = decode_float(cp,optlen);
      break;
    case TP2:
      chan->tp2 = decode_float(cp,optlen);
      break;
    case SQUELCH_OPEN:
      chan->squelch.open = dB2power(decode_float(cp,optlen));
      break;
    case SQUELCH_CLOSE:
      chan->squelch.close = dB2power(decode_float(cp,optlen));
      break;
    case DEEMPH_GAIN:
      chan->fm.gain = decode_float(cp,optlen);
      break;
    case DEEMPH_TC:
      chan->fm.rate = 1e6*decode_float(cp,optlen);
      break;
    case PL_TONE:
      chan->fm.tone_freq = decode_float(cp,optlen);
      break;
    case PL_DEVIATION:
      chan->fm.tone_deviation = decode_float(cp,optlen);
      break;
    case RESOLUTION_BW:
      chan->spectrum.rbw = decode_float(cp,optlen);
      break;
    case SPECTRUM_AVG:
      chan->spectrum.fft_avg = decode_int(cp,optlen);
      break;
    case BIN_COUNT:
      chan->spectrum.bin_count = decode_int(cp,optlen);
      break;
    case CROSSOVER:
      chan->spectrum.crossover = decode_float(cp,optlen);
      break;
    case WINDOW_TYPE:
      chan->spectrum.window_type = decode_int(cp,optlen);
      break;
    case SPECTRUM_SHAPE:
      chan->spectrum.shape = decode_float(cp,optlen);
      break;
    case SPECTRUM_FFT_N:
      chan->spectrum.fft_n = decode_int(cp,optlen);
      break;
    case SPECTRUM_BASE:
      chan->spectrum.base = decode_float(cp,optlen);
      break;
    case SPECTRUM_STEP:
      chan->spectrum.step = decode_float(cp,optlen);
      break;
    case BIN_DATA:
      break;
    case BIN_BYTE_DATA:
      break;
    case RF_AGC:
      frontend->rf_agc = decode_int(cp,optlen);
      break;
    case RF_GAIN:
      frontend->rf_gain = decode_float(cp,optlen);
      break;
    case RF_ATTEN:
      frontend->rf_atten = decode_float(cp,optlen);
      break;
    case RF_LEVEL_CAL:
      frontend->rf_level_cal = decode_float(cp,optlen);
      break;
    case RTP_PT:
      chan->output.rtp.type = decode_int8(cp,optlen);
      break;
    case OUTPUT_ENCODING:
      chan->output.encoding = decode_int(cp,optlen);
      break;
    case STATUS_INTERVAL:
      chan->status.output_interval = decode_int(cp,optlen);
      break;
    case SETOPTS:
      chan->options = decode_int64(cp,optlen);
      break;
    case OPUS_BIT_RATE:
      chan->opus.bitrate = decode_int(cp,optlen);
      break;
    case OPUS_DTX:
      chan->opus.dtx = decode_bool(cp,optlen);
      break;
    case OPUS_APPLICATION:
      chan->opus.application = decode_int(cp,optlen);
      break;
    case OPUS_FEC:
      chan->opus.fec = decode_int(cp,optlen);
      break;
    case OPUS_BANDWIDTH:
      chan->opus.bandwidth = decode_int(cp,optlen);
      break;
    case MAXDELAY:
      chan->output.maxdelay = decode_int(cp,optlen);
      break;
    case FILTER2:
      chan->filter2.blocking = decode_int(cp,optlen);
      break;
    case OUTPUT_ERRORS:
      chan->output.errors = decode_int64(cp,optlen);
      break;
    case FILTER2_BLOCKSIZE:
      chan->filter2.in.ilen = decode_int(cp,optlen);
      break;
    case FILTER2_FIR_LENGTH:
      chan->filter2.in.impulse_length = decode_int(cp,optlen);
      break;
    case FILTER2_KAISER_BETA:
      chan->filter2.kaiser_beta = decode_float(cp,optlen);
      break;
    case NOISE_BW:
      chan->spectrum.noise_bw = decode_float(cp,optlen);
      break;
    case SPECTRUM_OVERLAP:
      chan->spectrum.overlap = decode_float(cp,optlen);
      break;
    case LIFETIME:
      chan->lifetime = decode_int(cp,optlen);
      break;
    case IQ_IMBALANCE:
      frontend->gain_error = decode_float(cp,optlen);
      break;
    case IQ_PHASE:
      frontend->phase_error = decode_float(cp,optlen);
      break;
    default: // ignore others
      break;
    }
    cp += optlen;
  }
  return 0;
}
// Extract SSRC; 0 means not present (reserved value)
uint32_t get_ssrc(uint8_t const *buffer,int length){
  if(length < 2)
    return 0;
  uint8_t const *cp = buffer;
  while(cp < &buffer[length-1]){
    enum status_type const type = *cp++; // increment cp to length field
    if(type == EOL)
      break; // end of list, no length
    unsigned int optlen = *cp++;
    if(optlen & 0x80){
      // length is >= 128 bytes; fetch actual length from next N bytes, where N is low 7 bits of optlen
      int length_of_length = optlen & 0x7f;
      if(length_of_length > (int)sizeof(optlen) || cp + length_of_length > &buffer[length])
	break;
      optlen = 0;
      while(length_of_length > 0){
	optlen <<= 8;
	optlen |= *cp++;
	length_of_length--;
      }
    }
    if(cp + optlen >= &buffer[length])
      break; // invalid length; we can't continue to scan
    switch(type){
    case EOL: // Shouldn't get here
      goto done;
    case OUTPUT_SSRC:
      return decode_int32(cp,optlen);
      break;
    default:
      break; // Ignore on this pass
    }
    cp += optlen;
  }
 done:;
  return 0;
}
// Extract command tag
uint32_t get_tag(uint8_t const *buffer,int length){
  if(length < 2)
    return 0;
  uint8_t const *cp = buffer;
  while(cp < &buffer[length-1]){
    enum status_type const type = *cp++; // increment cp to length field
    if(type == EOL)
      break; // end of list, no length
    unsigned int optlen = *cp++;
    if(optlen & 0x80){
      // length is >= 128 bytes; fetch actual length from next N bytes, where N is low 7 bits of optlen
      int length_of_length = optlen & 0x7f;
      if(length_of_length > (int)sizeof(optlen) || cp + length_of_length > &buffer[length])
	break;
      optlen = 0;
      while(length_of_length > 0){
	optlen <<= 8;
	optlen |= *cp++;
	length_of_length--;
      }
    }
    if(cp + optlen >= &buffer[length])
      break; // invalid length; we can't continue to scan
    switch(type){
    case EOL: // Shouldn't get here
      goto done;
    case COMMAND_TAG:
      return decode_int64(cp,optlen);
      break;
    default:
      break; // Ignore on this pass
    }
    cp += optlen;
  }
 done:;
  return 0; // broadcast
}
