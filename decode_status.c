#include <string.h>
#include "radio.h"

// Decode incoming status message from the radio program, convert and fill in fields in local channel structure
// Leave all other fields unchanged, as they may have local uses (e.g., file descriptors)
// Note that we use some fields in channel differently than in radiod (e.g., dB vs ratios)
int decode_radio_status(struct frontend *frontend,struct channel *channel,uint8_t const *buffer,int length){
  if(frontend == NULL || channel == NULL || buffer == NULL)
    return -1;
  uint8_t const *cp = buffer;
  while(cp - buffer < length){
    enum status_type type = *cp++; // increment cp to length field

    if(type == EOL)
      break; // end of list

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
    case EOL:
      break;
    case CMD_CNT:
      channel->status.packets_in = decode_int32(cp,optlen);
      break;
    case DESCRIPTION:
      FREE(frontend->description);
      frontend->description = decode_string(cp,optlen);
      break;
    case STATUS_DEST_SOCKET:
      decode_socket(&Metadata_dest_socket,cp,optlen);
      break;
    case GPS_TIME:
      frontend->timestamp = decode_int64(cp,optlen);
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
      decode_socket(&channel->output.source_socket,cp,optlen);
      break;
    case OUTPUT_DATA_DEST_SOCKET:
      decode_socket(&channel->output.dest_socket,cp,optlen);
      break;
    case OUTPUT_SSRC:
      channel->output.rtp.ssrc = decode_int32(cp,optlen);
      break;
    case OUTPUT_TTL:
      Mcast_ttl = decode_int8(cp,optlen);
      break;
    case OUTPUT_SAMPRATE:
      channel->output.samprate = decode_int(cp,optlen);
      break;
    case OUTPUT_DATA_PACKETS:
      channel->output.rtp.packets = decode_int64(cp,optlen);
      break;
    case OUTPUT_METADATA_PACKETS:
      channel->status.packets_out = decode_int64(cp,optlen);
      break;
    case FILTER_BLOCKSIZE:
      frontend->L = decode_int(cp,optlen);
      break;
    case FILTER_FIR_LENGTH:
      frontend->M = decode_int(cp,optlen);
      break;
    case LOW_EDGE:
      channel->filter.min_IF = decode_float(cp,optlen);
      break;
    case HIGH_EDGE:
      channel->filter.max_IF = decode_float(cp,optlen);
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
      channel->filter.kaiser_beta = decode_float(cp,optlen);
      break;
    case FILTER_DROPS:
      channel->filter.out.block_drops = decode_int(cp,optlen);
      break;
    case IF_POWER:
      frontend->if_power = dB2power(decode_float(cp,optlen));
      break;
    case BASEBAND_POWER:
      channel->sig.bb_power = dB2power(decode_float(cp,optlen)); // dB -> power
      break;
    case NOISE_DENSITY:
      channel->sig.n0 = dB2power(decode_float(cp,optlen));
      break;
    case DEMOD_SNR:
      channel->sig.snr = dB2power(decode_float(cp,optlen));
      break;
    case FREQ_OFFSET:
      channel->sig.foffset = decode_float(cp,optlen);
      break;
    case PEAK_DEVIATION:
      channel->fm.pdeviation = decode_float(cp,optlen);
      break;
    case PLL_LOCK:
      channel->pll.lock = decode_bool(cp,optlen);
      break;
    case PLL_BW:
      channel->pll.loop_bw = decode_float(cp,optlen);
      break;
    case PLL_SQUARE:
      channel->pll.square = decode_bool(cp,optlen);
      break;
    case PLL_PHASE:
      channel->pll.cphase = decode_float(cp,optlen);
      break;
    case PLL_WRAPS:
      channel->pll.rotations = (int64_t)decode_int64(cp,optlen);
      break;
    case ENVELOPE:
      channel->linear.env = decode_bool(cp,optlen);
      break;
    case OUTPUT_LEVEL:
      channel->output.energy = dB2power(decode_float(cp,optlen));
      break;
    case OUTPUT_SAMPLES:
      channel->output.samples = decode_int64(cp,optlen);
      break;
    case COMMAND_TAG:
      channel->status.tag = decode_int32(cp,optlen);
      break;
    case RADIO_FREQUENCY:
      channel->tune.freq = decode_double(cp,optlen);
      break;
    case SECOND_LO_FREQUENCY:
      channel->tune.second_LO = decode_double(cp,optlen);
      break;
    case SHIFT_FREQUENCY:
      channel->tune.shift = decode_double(cp,optlen);
      break;
    case FIRST_LO_FREQUENCY:
      frontend->frequency = decode_double(cp,optlen);
      break;
    case DOPPLER_FREQUENCY:
      channel->tune.doppler = decode_double(cp,optlen);
      break;
    case DOPPLER_FREQUENCY_RATE:
      channel->tune.doppler_rate = decode_double(cp,optlen);
      break;
    case DEMOD_TYPE:
      channel->demod_type = decode_int(cp,optlen);
      break;
    case OUTPUT_CHANNELS:
      channel->output.channels = decode_int(cp,optlen);
      break;
    case INDEPENDENT_SIDEBAND:
      channel->filter2.isb = decode_bool(cp,optlen);
      break;
    case THRESH_EXTEND:
      channel->fm.threshold = decode_bool(cp,optlen);
      break;
    case PLL_ENABLE:
      channel->pll.enable = decode_bool(cp,optlen);
      break;
    case GAIN:              // dB to voltage
      channel->output.gain = dB2voltage(decode_float(cp,optlen));
      break;
    case AGC_ENABLE:
      channel->linear.agc = decode_bool(cp,optlen);
      break;
    case HEADROOM:          // db to voltage
      channel->output.headroom = dB2voltage(decode_float(cp,optlen));
      break;
    case AGC_HANGTIME:      // s to samples
      channel->linear.hangtime = decode_float(cp,optlen);
      break;
    case AGC_RECOVERY_RATE: // dB/s to dB/sample to voltage/sample
      channel->linear.recovery_rate = dB2voltage(decode_float(cp,optlen));
      break;
    case AGC_THRESHOLD:   // dB to voltage
      channel->linear.threshold = dB2voltage(decode_float(cp,optlen));
      break;
    case TP1: // Test point
      channel->tp1 = decode_float(cp,optlen);
      break;
    case TP2:
      channel->tp2 = decode_float(cp,optlen);
      break;
    case SQUELCH_OPEN:
      channel->fm.squelch_open = dB2power(decode_float(cp,optlen));
      break;
    case SQUELCH_CLOSE:
      channel->fm.squelch_close = dB2power(decode_float(cp,optlen));
      break;
    case DEEMPH_GAIN:
      channel->fm.gain = decode_float(cp,optlen);
      break;
    case DEEMPH_TC:
      channel->fm.rate = 1e6*decode_float(cp,optlen);
      break;
    case PL_TONE:
      channel->fm.tone_freq = decode_float(cp,optlen);
      break;
    case PL_DEVIATION:
      channel->fm.tone_deviation = decode_float(cp,optlen);
      break;
    case NONCOHERENT_BIN_BW:
      channel->spectrum.bin_bw = decode_float(cp,optlen);
      break;
    case BIN_COUNT:
      channel->spectrum.bin_count = decode_int(cp,optlen);
      break;
    case BIN_DATA:
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
    case BLOCKS_SINCE_POLL:
      channel->status.blocks_since_poll = decode_int64(cp,optlen);
      break;
    case PRESET:
      {
	char *p = decode_string(cp,optlen);
	strlcpy(channel->preset,p,sizeof(channel->preset));
	FREE(p);
      }
      break;
    case RTP_PT:
      channel->output.rtp.type = decode_int(cp,optlen);
      break;
    case OUTPUT_ENCODING:
      channel->output.encoding = decode_int(cp,optlen);
      break;
    case STATUS_INTERVAL:
      channel->status.output_interval = decode_int(cp,optlen);
      break;
    case SETOPTS:
      channel->options = decode_int64(cp,optlen);
      break;
    case OPUS_BIT_RATE:
      channel->output.opus_bitrate = decode_int(cp,optlen);
      break;
    case MINPACKET:
      channel->output.minpacket = decode_int(cp,optlen);
      break;
    case FILTER2:
      channel->filter2.blocking = decode_int(cp,optlen);
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
  uint8_t const *cp = buffer;

  while(cp - buffer < length){
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
      return decode_int32(cp,optlen);
      break;
    default:
      break; // Ignore on this pass
    }
    cp += optlen;
  }
 done:;
  return 0; // broadcast
}
