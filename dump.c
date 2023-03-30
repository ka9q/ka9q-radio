// $Id: dump.c,v 1.41 2023/02/23 23:46:49 karn Exp $
#define _GNU_SOURCE 1
#include <assert.h>
#include <stdio.h>
#include <math.h>
#include <netdb.h>
#include <stdint.h>

#include "misc.h"
#include "status.h"
#include "multicast.h"
#include "radio.h"

void dump_metadata(uint8_t const * const buffer,int length){
  uint8_t const *cp = buffer;

  while(cp - buffer < length){
    enum status_type const type = *cp++; // increment cp to length field
    
    if(type == EOL)
      break; // End of list

    unsigned int const optlen = *cp++;
    if(cp - buffer + optlen >= length)
      break; // Invalid length
    printf(" (%d) ",type);
    switch(type){
    case EOL: // Shouldn't get here
      goto done;
    case COMMAND_TAG:
      printf("cmd tag %llx",(long long unsigned)decode_int(cp,optlen));
      break;
    case CMD_CNT:
      printf("commands %'llu",(long long unsigned)decode_int(cp,optlen));
      break;
    case GPS_TIME:
      {
	char tbuf[100];
	printf("%s",format_gpstime(tbuf,sizeof(tbuf),(uint64_t)decode_int(cp,optlen)));
      }
      break;
    case DESCRIPTION:
      {
	char sbuf[256];
	printf("%s",decode_string(cp,optlen,sbuf,sizeof(sbuf)));
      }
      break;
    case INPUT_DATA_SOURCE_SOCKET:
      {
	struct sockaddr_storage sock;
	printf("in data src %s",formatsock(decode_socket(&sock,cp,optlen)));
      }
      break;
    case INPUT_DATA_DEST_SOCKET:
      {
	struct sockaddr_storage sock;
	printf("in data dst %s",formatsock(decode_socket(&sock,cp,optlen)));
      }
      break;
    case INPUT_METADATA_SOURCE_SOCKET:
      {
	struct sockaddr_storage sock;
	printf("in metadata src %s",formatsock(decode_socket(&sock,cp,optlen)));
      }
      break;
    case INPUT_METADATA_DEST_SOCKET:      
      {
	struct sockaddr_storage sock;
	printf("in metadata dst %s",formatsock(decode_socket(&sock,cp,optlen)));
      }
      break;
    case INPUT_SSRC:
      printf("in SSRC %'llu",(long long unsigned)decode_int(cp,optlen));
      break;
    case INPUT_SAMPRATE:
      printf("in samprate %'llu Hz",(long long unsigned)decode_int(cp,optlen));
      break;
    case INPUT_METADATA_PACKETS:
      printf("in metadata packets %'llu",(long long unsigned)decode_int(cp,optlen));
      break;
    case INPUT_DATA_PACKETS:
      printf("in data packets %'llu",(long long unsigned)decode_int(cp,optlen));
      break;
    case INPUT_SAMPLES:
      printf("in samples %'llu",(long long unsigned)decode_int(cp,optlen));
      break;
    case INPUT_DROPS:
      printf("in drops %'llu",(long long unsigned)decode_int(cp,optlen));
      break;
    case INPUT_DUPES:
      printf("in dupes %'llu",(long long unsigned)decode_int(cp,optlen));
      break;
    case OUTPUT_DATA_SOURCE_SOCKET:
      {
	struct sockaddr_storage sock;
	printf("out data src %s",formatsock(decode_socket(&sock,cp,optlen)));
      }
      break;
    case OUTPUT_DATA_DEST_SOCKET:
      {
	struct sockaddr_storage sock;
	printf("out data dst %s",formatsock(decode_socket(&sock,cp,optlen)));
      }
      break;
    case OUTPUT_SSRC:
      printf("out SSRC %'llu",(long long unsigned)decode_int(cp,optlen));
      break;
    case OUTPUT_TTL:
      printf("out TTL %'llu",(long long unsigned)decode_int(cp,optlen));
      break;
    case OUTPUT_SAMPRATE:
      printf("out samprate %'llu Hz",(long long unsigned)decode_int(cp,optlen));
      break;
    case OUTPUT_METADATA_PACKETS:
      printf("out metadata pkts %'llu",(long long unsigned)decode_int(cp,optlen));
      break;
    case OUTPUT_DATA_PACKETS:
      printf("out data pkts %'llu",(long long unsigned)decode_int(cp,optlen));
      break;
    case AD_LEVEL:
      printf("A/D level %.1f dB",decode_float(cp,optlen));
      break;
    case CALIBRATE:
      printf("calibration %'lg",decode_double(cp,optlen));
      break;
    case LNA_GAIN:
      printf("lna gain %'llu dB",(long long unsigned)decode_int(cp,optlen));
      break;
    case MIXER_GAIN:
      printf("mixer gain %'llu dB",(long long unsigned)decode_int(cp,optlen));
      break;
    case IF_GAIN:
      printf("if gain %'llu dB",(long long unsigned)decode_int(cp,optlen));
      break;
    case DC_I_OFFSET:
      printf("DC I offset %g",decode_float(cp,optlen));
      break;
    case DC_Q_OFFSET:
      printf("DC Q offset %g",decode_float(cp,optlen));
      break;
    case IQ_IMBALANCE:
      printf("gain imbal %.1f dB",decode_float(cp,optlen));
      break;
    case IQ_PHASE:
      printf("phase imbal %.1f deg",DEGPRA*asinf(decode_float(cp,optlen)));
      break;
    case DIRECT_CONVERSION:
      printf("direct conv %d",(int)decode_int(cp,optlen));
      break;
    case RADIO_FREQUENCY:
      printf("RF %'.3lf Hz",decode_double(cp,optlen));
      break;
    case FIRST_LO_FREQUENCY:
      printf("first LO %'.3lf Hz",decode_double(cp,optlen));
      break;
    case SECOND_LO_FREQUENCY:
      printf("second LO %'.3lf Hz",decode_double(cp,optlen));
      break;
    case SHIFT_FREQUENCY:
      printf("shift %'.3lf Hz",decode_double(cp,optlen));
      break;
    case DOPPLER_FREQUENCY:
      printf("doppler %'.3lf Hz",decode_double(cp,optlen));
      break;
    case DOPPLER_FREQUENCY_RATE:
      printf("doppler rate %'.3lf Hz/s",decode_double(cp,optlen));
      break;
    case LOW_EDGE:
      printf("filt low %'g Hz",decode_float(cp,optlen));
      break;
    case HIGH_EDGE:
      printf("filt high %'g Hz",decode_float(cp,optlen));
      break;
    case KAISER_BETA:
      printf("filter kaiser_beta %g",decode_float(cp,optlen));      
      break;
    case FILTER_BLOCKSIZE:
      printf("filter L %'llu",(long long unsigned)decode_int(cp,optlen));
      break;
    case FILTER_FIR_LENGTH:
      printf("filter M %'llu",(long long unsigned)decode_int(cp,optlen));
      break;
    case NOISE_BANDWIDTH:
      printf("noise BW %'g Hz",decode_float(cp,optlen));
      break;
    case IF_POWER:
      printf("IF pwr %'.1f dB",decode_float(cp,optlen));
      break;
    case BASEBAND_POWER:
      printf("BB pwr %'.1f dB",decode_float(cp,optlen));
      break;
    case NOISE_DENSITY:
      printf("N0 %'.1f dB/Hz",decode_float(cp,optlen));
      break;
    case DEMOD_TYPE:
      {
	const int i = (int)decode_int(cp,optlen); // ????
	printf("demod %d ",i);
	switch(i){
	case LINEAR_DEMOD:
	  printf("(linear)");
	  break;
	case FM_DEMOD:
	  printf("(FM)");
	  break;
	case WFM_DEMOD:
	  printf("(wide FM)");
	  break;
	case SPECT_DEMOD:
	  printf("(spectrum)");
	  break;
	default:
	  printf("(unknown)");
	  break;
	}
      }
      break;
    case OUTPUT_CHANNELS:
      printf("out channels %'llu",(long long unsigned)decode_int(cp,optlen));
      break;
    case INDEPENDENT_SIDEBAND:
      printf("ISB %'llu",(long long unsigned)decode_int(cp,optlen));
      break;
    case THRESH_EXTEND:
      printf("Thr Extend %'llu",(long long unsigned)decode_int(cp,optlen));
      break;
    case PLL_ENABLE:
      printf("PLL enable %'llu",(long long unsigned)decode_int(cp,optlen));
      break;
    case PLL_LOCK:
      printf("PLL lock %'llu",(long long unsigned)decode_int(cp,optlen));
      break;
    case PLL_SQUARE:
      printf("PLL square %'llu",(long long unsigned)decode_int(cp,optlen));
      break;
    case PLL_PHASE:
      printf("PLL phase %g deg",DEGPRA*decode_float(cp,optlen));
      break;
    case PLL_BW:
      printf("PLL loop BW %'.1f Hz",decode_float(cp,optlen));
      break;
    case ENVELOPE:
      printf("Env det %llu",(long long unsigned)decode_int(cp,optlen));      
      break;
    case FM_FLAT:
      printf("FM flat %llu",(long long unsigned)decode_int(cp,optlen));
      break;
    case DEMOD_SNR:
      printf("Demod SNR %.1f dB",decode_float(cp,optlen));
      break;
    case FREQ_OFFSET:
      printf("freq offset %'g Hz",decode_float(cp,optlen));
      break;
    case PEAK_DEVIATION:
      printf("peak FM dev %'g Hz",decode_float(cp,optlen));
      break;
    case PL_TONE:
      printf("PL tone freq %'g Hz",decode_float(cp,optlen));
      break;
    case PL_DEVIATION:
      printf("PL tone deviation %'g Hz",decode_float(cp,optlen));
      break;
    case AGC_ENABLE:
      printf("agc enab %'llu",(long long unsigned)decode_int(cp,optlen));
      break;
    case HEADROOM:
      printf("headroom %.1f dB",decode_float(cp,optlen));
      break;
    case AGC_HANGTIME:
      printf("hangtime %'g s",decode_float(cp,optlen));
      break;
    case AGC_RECOVERY_RATE:
      printf("recovery rate %.1f dB/s",decode_float(cp,optlen));
      break;
    case AGC_ATTACK_RATE:
      printf("attack rate %.1f dB/s",decode_float(cp,optlen));
      break;
    case AGC_THRESHOLD:
      printf("threshold %.1f dB",decode_float(cp,optlen));
      break;
    case GAIN:
      printf("gain %.1f dB",decode_float(cp,optlen));
      break;
    case OUTPUT_LEVEL:
      printf("output level %.1f dB",decode_float(cp,optlen));
      break;
    case OUTPUT_SAMPLES:
      printf("output samp %'llu",(long long unsigned)decode_int(cp,optlen));
      break;
    case OPUS_SOURCE_SOCKET:
      {
	struct sockaddr_storage sock;
	printf("opus src %s",formatsock(decode_socket(&sock,cp,optlen)));
      }
      break;
    case OPUS_DEST_SOCKET:
      {
	struct sockaddr_storage sock;
	printf("opus dst %s",formatsock(decode_socket(&sock,cp,optlen)));
      }
      break;
    case OPUS_SSRC:
      printf("opus ssrc %'u",(int)decode_int(cp,optlen));
      break;
    case OPUS_TTL:
      printf("opus ttl %d",(int)decode_int(cp,optlen));
      break;
    case OPUS_BITRATE:
      printf("opus rate %'d bps",(int)decode_int(cp,optlen));
      break;
    case OPUS_PACKETS:
      printf("opus pkts %'llu",(long long unsigned)decode_int(cp,optlen));
      break;
    case FILTER_DROPS:
      printf("block drops %'llu",(long long unsigned)decode_int(cp,optlen));
      break;
    case LOCK:
      printf("lock %llu",(long long unsigned)decode_int(cp,optlen));
      break;
    case TP1:
      printf("TP1 %'.1f",decode_float(cp,optlen));
      break;
    case TP2:
      printf("TP2 %'.1f",decode_float(cp,optlen));
      break;
    case GAINSTEP:
      printf("gain step %'llu",(long long unsigned)decode_int(cp,optlen));
      break;
    case OUTPUT_BITS_PER_SAMPLE:
      printf("output bits/sample %d",(int)decode_int(cp,optlen));
      break;
    case SQUELCH_OPEN:
      printf("squelch open %.1f dB",decode_float(cp,optlen));
      break;
    case SQUELCH_CLOSE:
      printf("squelch close %.1f dB",decode_float(cp,optlen));
      break;
    case DEEMPH_GAIN:
      printf("deemph gain %.1f dB",decode_float(cp,optlen));
      break;
    case DEEMPH_TC:
      printf("demph tc %.1f us",1e6f * decode_float(cp,optlen));
      break;
    case CONVERTER_OFFSET:
      printf("converter %.1f Hz",decode_float(cp,optlen));
      break;
    case PRESET:
      {
	char sbuf[256];
	printf("preset %s",decode_string(cp,optlen,sbuf,sizeof(sbuf)));      
      }
      break;
    case COHERENT_BIN_BW:
      printf("coherent bin bandwidth %.1f Hz",decode_float(cp,optlen));
      break;
    case COHERENT_BIN_SPACING:
      printf("coherent bin spacing %.1f Hz",decode_float(cp,optlen));
      break;
    case NONCOHERENT_BIN_BW:
      printf("noncoherent bin bandwidth %.1f Hz",decode_float(cp,optlen));
      break;
    case BIN_COUNT:
      printf("bin count %d",(int)decode_int(cp,optlen));
      break;
    case INTEGRATE_TC:
      printf("integrate tc %.1f s",decode_float(cp,optlen));
      break;
    case BIN_DATA:
      {
	printf("bin data:");
	int count = optlen/sizeof(float);
	for(int i=0; i < count; i++){
	  printf(" %.0f",decode_float(cp,sizeof(float)));
	  cp += sizeof(float);
	}
      }
      break;
    default:
      printf("unknown type %d length %d",type,optlen);
      break;
    }
    cp += optlen;
  }
 done:;
  printf("\n");
}
