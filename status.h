#ifndef _STATUS_H
#define _STATUS_H 1
#include <stdint.h>
#include <stdint.h>
#include <sys/time.h>


enum status_type {
  EOL = 0,	  
  COMMAND_TAG,    // Echoes tag from requester
  CMD_CNT,       // Count of input commands
  GPS_TIME,       // Nanoseconds since GPS epoch (remember to update the leap second tables!)

  DESCRIPTION,    // Free form text describing source
  INPUT_DATA_SOURCE_SOCKET,
  INPUT_DATA_DEST_SOCKET,
  INPUT_METADATA_SOURCE_SOCKET,
  INPUT_METADATA_DEST_SOCKET,
  INPUT_SSRC,
  INPUT_SAMPRATE, // Nominal sample rate (integer)
  INPUT_METADATA_PACKETS,
  INPUT_DATA_PACKETS,
  INPUT_SAMPLES,
  INPUT_DROPS,
  INPUT_DUPES,

  OUTPUT_DATA_SOURCE_SOCKET,
  OUTPUT_DATA_DEST_SOCKET,
  OUTPUT_SSRC,
  OUTPUT_TTL,
  OUTPUT_SAMPRATE,
  OUTPUT_METADATA_PACKETS,
  OUTPUT_DATA_PACKETS,

  // Hardware
  AD_LEVEL,    // NO LONGER USED
  CALIBRATE,
  // Hardware-specific analog gains
  LNA_GAIN,
  MIXER_GAIN,
  IF_GAIN,

  DC_I_OFFSET,
  DC_Q_OFFSET,
  IQ_IMBALANCE,
  IQ_PHASE,
  DIRECT_CONVERSION, // Boolean indicating SDR is direct conversion -- should avoid DC

  // Tuning
  RADIO_FREQUENCY,
  FIRST_LO_FREQUENCY,
  SECOND_LO_FREQUENCY,
  SHIFT_FREQUENCY,
  DOPPLER_FREQUENCY,
  DOPPLER_FREQUENCY_RATE,

  // Filtering
  LOW_EDGE,
  HIGH_EDGE,
  KAISER_BETA,
  FILTER_BLOCKSIZE,
  FILTER_FIR_LENGTH,
  NOISE_BANDWIDTH,

  // Signals
  IF_POWER,
  BASEBAND_POWER,
  NOISE_DENSITY,

  // Demodulation configuration
  DEMOD_TYPE, // 0 = linear (default), 1 = FM, 2 = WFM/Stereo, 3 = spectrum
  OUTPUT_CHANNELS, // 1 or 2 in Linear, otherwise 1
  INDEPENDENT_SIDEBAND, // Linear only
  PLL_ENABLE,
  PLL_LOCK,       // Linear PLL
  PLL_SQUARE,     // Linear PLL
  PLL_PHASE,      // Linear PLL
  PLL_BW,         // PLL loop bandwidth
  ENVELOPE,       // Envelope detection in linear mode
  FM_FLAT,
  
  // Demodulation status
  DEMOD_SNR,      // FM, PLL linear
  FREQ_OFFSET,    // FM, PLL linear
  PEAK_DEVIATION, // FM only
  PL_TONE,        // PL tone squelch frequency (FM only)
  
  // Settable gain parameters
  AGC_ENABLE,     // boolean, linear modes only
  HEADROOM,       // Audio level headroom, stored as amplitude ratio, exchanged as dB
  AGC_HANGTIME,   // AGC hang time, stored as samples, exchanged as sec
  AGC_RECOVERY_RATE, // stored as amplitude ratio/sample, exchanged as dB/sec
  AGC_ATTACK_RATE, // stored as amplitude ratio/sample, exchanged as dB/sec
  AGC_THRESHOLD,   // stored as amplitude ratio, exchanged as dB

  GAIN,     // AM, Linear only, stored as amplitude ratio, exchanged as dB
  OUTPUT_LEVEL,     // All modes
  OUTPUT_SAMPLES,

  OPUS_SOURCE_SOCKET,
  OPUS_DEST_SOCKET,
  OPUS_SSRC,
  OPUS_TTL,
  OPUS_BITRATE,
  OPUS_PACKETS,

  FILTER_DROPS,
  LOCK,     // Tuner is locked, will ignore retune commands (boolean)

  TP1, // General purpose test points (floating point)
  TP2,

  GAINSTEP,
  OUTPUT_BITS_PER_SAMPLE,
  SQUELCH_OPEN,   // Squelch opening threshold SNR
  SQUELCH_CLOSE,  // and closing
  PRESET,         // char string containing mode presets
  DEEMPH_TC,      // De-emphasis time constant (FM only)
  DEEMPH_GAIN,    // De-emphasis gain (FM only)
  CONVERTER_OFFSET, // Frequency converter shift (if present)
  PL_DEVIATION,     // Measured PL tone deviation, Hz (FM only)
  THRESH_EXTEND,    // threshold extension enable (FM only)

  // Spectral analysis
  COHERENT_BIN_BW,  // block rate in Hz
  COHERENT_BIN_SPACING, // (1-overlap) * block rate = (1 - ((M-1)/(L+M-1))) * block rate
  NONCOHERENT_BIN_BW, // Bandwidth (Hz) of noncoherent integration bin, some multiple of COHERENT_BIN_SPACING
  BIN_COUNT,        // Integer number of bins accumulating energy noncoherently
  INTEGRATE_TC,   // Integration time constant, sec
  BIN_DATA,         // Vector of relative bin energies, real (I^2 + Q^2)

  RF_ATTEN,       // Front end attenuation (introduced with rx888)

};

int encode_string(uint8_t **bp,enum status_type type,void const *buf,unsigned int buflen);
int encode_eol(uint8_t **buf);
int encode_byte(uint8_t **buf,enum status_type type,uint8_t x);
int encode_int(uint8_t **buf,enum status_type type,int x);
int encode_int16(uint8_t **buf,enum status_type type,uint16_t x);
int encode_int32(uint8_t **buf,enum status_type type,uint32_t x);
int encode_int64(uint8_t **buf,enum status_type type,uint64_t x);
int encode_float(uint8_t **buf,enum status_type type,float x);
int encode_double(uint8_t **buf,enum status_type type,double x);
int encode_socket(uint8_t **buf,enum status_type type,void const *sock);
int encode_vector(uint8_t **buf,enum status_type type,float *array,int size);

uint64_t decode_int(uint8_t const *,int);
float decode_float(uint8_t const *,int);
double decode_double(uint8_t const *,int);
struct sockaddr *decode_socket(void *sock,uint8_t const *,int);
char *decode_string(uint8_t const *,int,char *,int);
uint32_t get_ssrc(uint8_t const *buffer,int length);
uint32_t get_tag(uint8_t const *buffer,int length);

void dump_metadata(uint8_t const *,int);

int64_t random_time(int64_t base,int64_t rrange);
uint32_t send_poll(int fd,int ssrc);

#endif
