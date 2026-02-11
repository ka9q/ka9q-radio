// Decode status packets from radiod into something human readable
// Copyright 2017-2023 Phil Karn, KA9Q

#define _GNU_SOURCE 1
#include <assert.h>
#include <stdio.h>
#include <math.h>
#include <netdb.h>
#include <stdint.h>
#include <opus/opus.h>

#include "misc.h"
#include "status.h"
#include "multicast.h"
#include "radio.h"
#include "window.h"

void dump_metadata(FILE *fp,uint8_t const * const buffer,size_t length,bool newline){
  uint8_t const *cp = buffer;

  while(cp < buffer + length){
    enum status_type const type = *cp++; // increment cp to length field

    if(type == EOL)
      break; // End of list

    int optlen = *cp++;
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
      break; // Invalid length
    fprintf(fp,"%s[%d] ",newline? "\n":" ",type);
    switch(type){
    case EOL: // Shouldn't get here
      goto done;
    case COMMAND_TAG:
      fprintf(fp,"cmd tag %08x",(uint32_t)decode_int32(cp,optlen));
      break;
    case CMD_CNT:
      fprintf(fp,"commands %'llu",(long long unsigned int)decode_int32(cp,optlen));
      break;
    case GPS_TIME:
      {
	char tbuf[100];
	fprintf(fp,"%s",format_gpstime(tbuf,sizeof(tbuf),(int64_t)decode_int64(cp,optlen))); // not likely negative anyway
      }
      break;
    case DESCRIPTION:
      {
	char *d = decode_string(cp,optlen);
	fprintf(fp,"%s",d);
	FREE(d);
      }
      break;
    case RTP_TIMESNAP:
      fprintf(fp,"RTP time snap %'u",decode_int32(cp,optlen));
      break;
    case STATUS_DEST_SOCKET:
      {
	struct sockaddr_storage sock;
	fprintf(fp,"status dest %s",formatsock(decode_socket(&sock,cp,optlen),true));
      }
      break;
      break;
    case INPUT_SAMPRATE:
      fprintf(fp,"in samprate %'llu Hz",(long long unsigned int)decode_int64(cp,optlen));
      break;
    case INPUT_SAMPLES:
      fprintf(fp,"in samples %'llu",(long long unsigned int)decode_int64(cp,optlen));
      break;
    case OUTPUT_DATA_SOURCE_SOCKET:
      {
	struct sockaddr_storage sock;
	fprintf(fp,"data src %s",formatsock(decode_socket(&sock,cp,optlen),true));
      }
      break;
    case OUTPUT_DATA_DEST_SOCKET:
      {
	struct sockaddr_storage sock;
	fprintf(fp,"data dst %s",formatsock(decode_socket(&sock,cp,optlen),true));
      }
      break;
    case OUTPUT_SSRC:
      fprintf(fp,"SSRC %'u",(unsigned int)decode_int32(cp,optlen));
      break;
    case OUTPUT_TTL:
      fprintf(fp,"TTL %'u",(unsigned int)decode_int8(cp,optlen));
      break;
    case OUTPUT_SAMPRATE:
      fprintf(fp,"samprate %'u Hz",(unsigned int)decode_int(cp,optlen));
      break;
    case OUTPUT_METADATA_PACKETS:
      fprintf(fp,"metadata pkts %'llu",(long long unsigned int)decode_int64(cp,optlen));
      break;
    case OUTPUT_DATA_PACKETS:
      fprintf(fp,"data pkts %'llu",(long long unsigned int)decode_int64(cp,optlen));
      break;
    case AD_OVER:
      fprintf(fp,"A/D overrange: %'llu",(long long unsigned int)decode_int64(cp,optlen));
      break;
    case SAMPLES_SINCE_OVER:
      fprintf(fp,"Samples since A/D overrange: %'llu",(long long unsigned int)decode_int64(cp,optlen));
      break;
    case CALIBRATE:
      fprintf(fp,"calibration %'lg",decode_double(cp,optlen));
      break;
    case LNA_GAIN:
      fprintf(fp,"lna gain %'d dB",decode_int(cp,optlen));
      break;
    case MIXER_GAIN:
      fprintf(fp,"mixer gain %'d dB",decode_int(cp,optlen));
      break;
    case IF_GAIN:
      fprintf(fp,"if gain %'d dB",decode_int(cp,optlen));
      break;
    case DC_I_OFFSET:
      fprintf(fp,"DC I offset %lg",decode_float(cp,optlen));
      break;
    case DC_Q_OFFSET:
      fprintf(fp,"DC Q offset %lg",decode_float(cp,optlen));
      break;
    case IQ_IMBALANCE:
      fprintf(fp,"gain imbal %.1lf dB",decode_float(cp,optlen));
      break;
    case IQ_PHASE:
      fprintf(fp,"phase imbal %.1lf deg",DEGPRA*asin(decode_float(cp,optlen)));
      break;
    case DIRECT_CONVERSION:
      fprintf(fp,"direct conv %s",decode_bool(cp,optlen) ? "yes" : "no");
      break;
    case RADIO_FREQUENCY:
      fprintf(fp,"RF %'.3lf Hz",decode_double(cp,optlen));
      break;
    case FIRST_LO_FREQUENCY:
      fprintf(fp,"first LO %'.3lf Hz",decode_double(cp,optlen));
      break;
    case SECOND_LO_FREQUENCY:
      fprintf(fp,"second LO %'.3lf Hz",decode_double(cp,optlen));
      break;
    case SHIFT_FREQUENCY:
      fprintf(fp,"shift %'.3lf Hz",decode_double(cp,optlen));
      break;
    case DOPPLER_FREQUENCY:
      fprintf(fp,"doppler %'.3lf Hz",decode_double(cp,optlen));
      break;
    case DOPPLER_FREQUENCY_RATE:
      fprintf(fp,"doppler rate %'.3lf Hz/s",decode_double(cp,optlen));
      break;
    case LOW_EDGE:
      fprintf(fp,"filt low %'lg Hz",decode_float(cp,optlen));
      break;
    case HIGH_EDGE:
      fprintf(fp,"filt high %'lg Hz",decode_float(cp,optlen));
      break;
    case FE_LOW_EDGE:
      fprintf(fp,"fe filt low %'lg Hz",decode_float(cp,optlen));
      break;
    case FE_HIGH_EDGE:
      fprintf(fp,"fe filt high %'lg Hz",decode_float(cp,optlen));
      break;
    case FE_ISREAL:
      fprintf(fp,"fe produces %s samples",decode_bool(cp,optlen) ? "real" : "complex");
      break;
    case WINDOW_TYPE:
      {
	enum window_type i = decode_int(cp,optlen);
	fprintf(fp,"fft window type %i ",i);
	switch(i){
	case KAISER_WINDOW:
	  fprintf(fp,"Kaiser");
	  break;
	case RECT_WINDOW:
	  fprintf(fp,"rect");
	  break;
	case BLACKMAN_WINDOW:
	  fprintf(fp,"Blackman");
	  break;
	case EXACT_BLACKMAN_WINDOW:
	  fprintf(fp,"exact Blackman");
	  break;
	case GAUSSIAN_WINDOW:
	  fprintf(fp,"Gaussian");
	  break;
	case HANN_WINDOW:
	  fprintf(fp,"Hann");
	  break;
	case HAMMING_WINDOW:
	  fprintf(fp,"Hamming");
	  break;
	default:
	  fprintf(fp,"unknown");
	  break;
	}
      }
      break;
    case SPECTRUM_SHAPE:
      fprintf(fp,"spectrum shape factor %lg",decode_float(cp,optlen));
      break;
    case KAISER_BETA:
      fprintf(fp,"filter kaiser_beta %lg",decode_float(cp,optlen));
      break;
    case FILTER_BLOCKSIZE:
      fprintf(fp,"filter L %'u",decode_int(cp,optlen));
      break;
    case FILTER_FIR_LENGTH:
      fprintf(fp,"filter M %'u",decode_int(cp,optlen));
      break;
    case IF_POWER:
      fprintf(fp,"IF pwr %'.1lf dB",decode_float(cp,optlen));
      break;
    case BASEBAND_POWER:
      fprintf(fp,"baseband pwr %'.1lf dB",decode_float(cp,optlen));
      break;
    case NOISE_DENSITY:
      fprintf(fp,"N0 %'.1lf dB/Hz",decode_float(cp,optlen));
      break;
    case DEMOD_TYPE:
      {
	const int i = decode_int(cp,optlen); // ????
	fprintf(fp,"demod %d ",i);
	switch(i){
	case LINEAR_DEMOD:
	  fprintf(fp,"(linear)");
	  break;
	case FM_DEMOD:
	  fprintf(fp,"(FM)");
	  break;
	case WFM_DEMOD:
	  fprintf(fp,"(wide FM)");
	  break;
	case SPECT_DEMOD:
	  fprintf(fp,"(spectrum)");
	  break;
	case SPECT2_DEMOD:
	  fprintf(fp,"(spectrum v2)");
	  break;
	default:
	  fprintf(fp,"(unknown)");
	  break;
	}
      }
      break;
    case OUTPUT_CHANNELS:
      fprintf(fp,"out channels %'u",decode_int(cp,optlen));
      break;
    case INDEPENDENT_SIDEBAND:
      fprintf(fp,"ISB %s",decode_bool(cp,optlen) ? "on" : "off");
      break;
    case THRESH_EXTEND:
      fprintf(fp,"Thr Extend %s",decode_bool(cp,optlen) ? "on" : "off");
      break;
    case PLL_ENABLE:
      fprintf(fp,"PLL %s",decode_bool(cp,optlen) ? "enable":"disable");
      break;
    case PLL_LOCK:
      fprintf(fp,"PLL %s",decode_bool(cp,optlen) ? "lock" : "unlock");
      break;
    case PLL_SQUARE:
      fprintf(fp,"PLL square %s",decode_bool(cp,optlen) ? "on" : "off");
      break;
    case PLL_PHASE:
      fprintf(fp,"PLL phase %lg deg",DEGPRA*decode_float(cp,optlen));
      break;
    case PLL_BW:
      fprintf(fp,"PLL loop BW %'.1lf Hz",decode_float(cp,optlen));
      break;
    case PLL_WRAPS:
      fprintf(fp,"PLL phase wraps %'lld",(long long)decode_int64(cp,optlen)); // signed?
      break;
    case SNR_SQUELCH:
      fprintf(fp,"SNR squelch %s",decode_bool(cp,optlen) ? "on" : "off");
     break;
    case ENVELOPE:
      fprintf(fp,"Env det %s",decode_bool(cp,optlen) ? "on" : "off");
      break;
    case PLL_SNR:
      fprintf(fp,"PLL SNR %.1lf dB",decode_float(cp,optlen));
      break;
    case FM_SNR:
      fprintf(fp,"FM SNR %.1lf dB",decode_float(cp,optlen));
      break;
    case FREQ_OFFSET:
      fprintf(fp,"freq offset %'lg Hz",decode_float(cp,optlen));
      break;
    case PEAK_DEVIATION:
      fprintf(fp,"peak FM dev %'lg Hz",decode_float(cp,optlen));
      break;
    case PL_TONE:
      fprintf(fp,"PL tone freq %'lg Hz",decode_float(cp,optlen));
      break;
    case PL_DEVIATION:
      fprintf(fp,"PL tone deviation %'lg Hz",decode_float(cp,optlen));
      break;
    case AGC_ENABLE:
      fprintf(fp,"channel agc %s",decode_bool(cp,optlen) ? "enable" : "disable");
      break;
    case HEADROOM:
      fprintf(fp,"headroom %.1lf dB",decode_float(cp,optlen));
      break;
    case AGC_HANGTIME:
      fprintf(fp,"hangtime %'lg s",decode_float(cp,optlen));
      break;
    case AGC_RECOVERY_RATE:
      fprintf(fp,"recovery rate %.1lf dB/s",decode_float(cp,optlen));
      break;
    case AGC_THRESHOLD:
      fprintf(fp,"threshold %.1lf dB",decode_float(cp,optlen));
      break;
    case GAIN:
      fprintf(fp,"gain %.1lf dB",decode_float(cp,optlen));
      break;
    case OUTPUT_LEVEL:
      fprintf(fp,"output level %+.1lf dBFS",decode_float(cp,optlen));
      break;
    case OUTPUT_SAMPLES:
      fprintf(fp,"output samp %'llu",(long long unsigned int)decode_int64(cp,optlen));
      break;
    case FILTER_DROPS:
      fprintf(fp,"block drops %'u",(unsigned int)decode_int(cp,optlen));
      break;
    case LOCK:
      fprintf(fp,"freq %s",decode_bool(cp,optlen) ? "locked" : "unlocked");
      break;
    case TP1:
      fprintf(fp,"TP1 %'.1lf",decode_float(cp,optlen));
      break;
    case TP2:
      fprintf(fp,"TP2 %'.1lf",decode_float(cp,optlen));
      break;
    case AD_BITS_PER_SAMPLE:
      fprintf(fp,"A/D bits/sample %u",decode_int(cp,optlen));
      break;
    case SQUELCH_OPEN:
      fprintf(fp,"squelch open %.1lf dB",decode_float(cp,optlen));
      break;
    case SQUELCH_CLOSE:
      fprintf(fp,"squelch close %.1lf dB",decode_float(cp,optlen));
      break;
    case DEEMPH_GAIN:
      fprintf(fp,"deemph gain %.1lf dB",decode_float(cp,optlen));
      break;
    case DEEMPH_TC:
      fprintf(fp,"demph tc %.1lf us",1.0e6 * decode_float(cp,optlen));
      break;
    case PRESET:
      {
	char *p = decode_string(cp,optlen);
	fprintf(fp,"preset %s",p);
	FREE(p);
      }
      break;
    case RESOLUTION_BW:
      fprintf(fp,"resolution bw %.1lf Hz",decode_float(cp,optlen));
      break;
    case SPECTRUM_AVG:
      fprintf(fp,"fft avg %u",decode_int(cp,optlen));
      break;
    case SPECTRUM_FFT_N:
      fprintf(fp,"fft N %u",decode_int(cp,optlen));
      break;
    case BIN_COUNT:
      fprintf(fp,"bins %u",decode_int(cp,optlen));
      break;
    case CROSSOVER:
      fprintf(fp,"crossover %.0lf",decode_float(cp,optlen));
      break;
    case SPECTRUM_OVERLAP:
      fprintf(fp,"fft overlap %.3lf",decode_float(cp,optlen));
      break;
    case RF_ATTEN:
      fprintf(fp,"rf atten %.1lf dB",decode_float(cp,optlen));
      break;
    case RF_GAIN:
      fprintf(fp,"rf gain %+.1lf dB",decode_float(cp,optlen));
      break;
    case RF_LEVEL_CAL:
      fprintf(fp,"rf level cal %+.1lf dBm",decode_float(cp,optlen));
      break;
    case RF_AGC:
      fprintf(fp,"rf agc %s",decode_bool(cp,optlen) ? "enabled" : "disabled");
      break;
    case BIN_DATA:
      {
	fprintf(fp,"fft bins:");
	int count = optlen/sizeof(float);
	for(int i=0; i < count; i++){
	  // Always float with length of 4 bytes
	  double x = decode_float(cp + i * sizeof(float),sizeof(float));
	  fprintf(fp," %.0lf",power2dB(x));
	}
      }
      break;
    case RTP_PT:
      fprintf(fp,"RTP PT %u",decode_int(cp,optlen));
      break;
    case STATUS_INTERVAL:
      fprintf(fp,"status interval %u",decode_int(cp,optlen));
      break;
    case OUTPUT_ENCODING:
      {
	enum encoding e = (enum encoding)decode_int(cp,optlen);
	fprintf(fp,"encoding %u (%s)",e,encoding_string(e));
      }
      break;
    case SETOPTS:
      {
	uint64_t opts = decode_int64(cp,optlen);
	fprintf(fp,"setopts 0x%llx",(unsigned long long)opts);
      }
      break;
    case CLEAROPTS:
      {
	uint64_t opts = decode_int64(cp,optlen);
	fprintf(fp,"clearopts 0x%llx",(unsigned long long)opts);
      }
      break;
    case OPUS_BIT_RATE:
      fprintf(fp,"opus bitrate %'u Hz",decode_int(cp,optlen));
      break;
    case OPUS_DTX:
      fprintf(fp,"opus dtx %s",decode_bool(cp,optlen) ? "on" : "off");
      break;
    case OPUS_APPLICATION:
      {
	char const *str = "unknown";
	int const x = (int)decode_int(cp,optlen);
	for(int i=0; Opus_application[i].str != NULL; i++){
	  if(Opus_application[i].value == x){
	    str = Opus_application[i].str;
	    break;
	  }
	}
	fprintf(fp,"opus application (%d) %s",x,str);
	break;
      }
    case OPUS_FEC:
      {
	unsigned int const x = decode_int(cp,optlen);
	fprintf(fp,"opus fec %u%%",x);
      }
      break;
    case OPUS_BANDWIDTH:
      {
	int const x = (int)decode_int(cp,optlen);
	const char *str = NULL;
	int bw = opus_bandwidth(&str,x);
	fprintf(fp,"opus bw %s (%d)",str,bw);
      }
      break;
    case MINPACKET:
      fprintf(fp,"minimum buffered pkts %u",decode_int(cp,optlen));
      break;
    case FILTER2:
      fprintf(fp,"filter2 blocks %u",decode_int(cp,optlen));
      break;
    case FILTER2_BLOCKSIZE:
      fprintf(fp,"filter2 blocksize %u",decode_int(cp,optlen));
      break;
    case FILTER2_FIR_LENGTH:
      fprintf(fp,"filter2 FIR length %u",decode_int(cp,optlen));
      break;
    case FILTER2_KAISER_BETA:
      fprintf(fp,"filter2 kaiser β %.1lf",decode_float(cp,optlen));
      break;
    case OUTPUT_ERRORS:
      fprintf(fp,"output errors %'llu",(unsigned long long)decode_int64(cp,optlen));
      break;
    case NOISE_BW:
      fprintf(fp,"bin noise bw %.1lf Hz",decode_float(cp,optlen));
      break;
    default:
      fprintf(fp,"unknown type %d length %d",type,optlen);
      break;
    }
    cp += optlen;
  }
 done:;
  fprintf(fp,"\n");
}
