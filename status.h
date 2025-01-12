// Type definitions for TLV encodings of status and commands from/to ka9q-radio radiod program
// Copyright 2017-2023, Phil Karn, KA9Q

#ifndef _STATUS_H
#define _STATUS_H 1
#include <stdio.h>
#include <stdint.h>
#include <stdint.h>
#include <sys/time.h>
#include <stdbool.h>

// Type field, first byte in command/status packets
enum pkt_type {
  STATUS = 0,
  CMD,
};

// I try not to delete or rearrange these entries since that makes the different programs incompatible
// with each other until they are all recompiled
enum status_type {
  EOL = 0,
  COMMAND_TAG,   // Echoes tag from requester
  CMD_CNT,       // Count of input commands
  GPS_TIME,      // Nanoseconds since GPS epoch (remember to update the leap second tables!)

  DESCRIPTION,   // Free form text describing source
  STATUS_DEST_SOCKET,
  SETOPTS,
  CLEAROPTS,
  UNUSED3,
  UNUSED4,
  INPUT_SAMPRATE, // Nominal sample rate (integer)
  UNUSED6,
  UNUSED7,
  INPUT_SAMPLES,
  UNUSED8,
  UNUSED9,

  OUTPUT_DATA_SOURCE_SOCKET,
  OUTPUT_DATA_DEST_SOCKET,
  OUTPUT_SSRC,
  OUTPUT_TTL,
  OUTPUT_SAMPRATE,
  OUTPUT_METADATA_PACKETS,
  OUTPUT_DATA_PACKETS,

  // Hardware
  UNUSED22,
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
  FILTER2,

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
  UNUSED18,

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
  UNUSED19,
  AGC_THRESHOLD,   // stored as amplitude ratio, exchanged as dB

  GAIN,     // AM, Linear only, stored as amplitude ratio, exchanged as dB
  OUTPUT_LEVEL,     // All modes
  OUTPUT_SAMPLES,

  OPUS_BIT_RATE,
  MINPACKET,      // Minimum number of full blocks in an output packet, unless already full (0-3)
  UNUSED13,
  UNUSED14,
  UNUSED15,
  UNUSED16,

  FILTER_DROPS,
  LOCK,     // Tuner is locked, will ignore retune commands (boolean)

  TP1, // General purpose test points (floating point)
  TP2,

  GAINSTEP,
  AD_BITS_PER_SAMPLE, // Front end A/D width, used for gain scaling
  SQUELCH_OPEN,   // Squelch opening threshold SNR
  SQUELCH_CLOSE,  // and closing
  PRESET,         // char string containing mode presets
  DEEMPH_TC,      // De-emphasis time constant (FM only)
  DEEMPH_GAIN,    // De-emphasis gain (FM only)
  CONVERTER_OFFSET, // Frequency converter shift (if present)
  PL_DEVIATION,     // Measured PL tone deviation, Hz (FM only)
  THRESH_EXTEND,    // threshold extension enable (FM only)

  // Spectral analysis
  UNUSED20,
  COHERENT_BIN_SPACING, // (1-overlap) * block rate = (1 - ((M-1)/(L+M-1))) * block rate
  NONCOHERENT_BIN_BW, // Bandwidth (Hz) of noncoherent integration bin, some multiple of COHERENT_BIN_SPACING
  BIN_COUNT,        // Integer number of bins accumulating energy noncoherently
  UNUSED21,
  BIN_DATA,         // Vector of relative bin energies, real (I^2 + Q^2)

  RF_ATTEN,       // Front end attenuation (introduced with rx888)
  RF_GAIN,        // Front end gain (introduced with rx888)
  RF_AGC,         // Front end AGC on/off
  FE_LOW_EDGE,    // edges of front end filter
  FE_HIGH_EDGE,
  FE_ISREAL,        // Boolean, true -> front end uses real sampling, false -> front end uses complex
  BLOCKS_SINCE_POLL,  // Blocks since last poll
  AD_OVER,          // A/D full scale samples, proxy for overranges
  RTP_PT,           // Real Time Protocol Payload Type
  STATUS_INTERVAL,      // Automatically send channel status over *data* channel every STATUS_RATE frames
  OUTPUT_ENCODING,    // Output data encoding (see enum encoding in multicast.h)
  SAMPLES_SINCE_OVER, // Samples since last A/D overrange
  PLL_WRAPS,          // Count of complete linear mode PLL rotations
  RF_LEVEL_CAL,        // Adjustment relating dBm to dBFS
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
int encode_vector(uint8_t **buf,enum status_type type,float const *array,int size);

uint64_t decode_int64(uint8_t const *,int);
uint32_t decode_int32(uint8_t const *,int);
uint16_t decode_int16(uint8_t const *,int);
uint8_t decode_int8(uint8_t const *,int);
bool decode_bool(uint8_t const *,int);
int decode_int(uint8_t const *,int);

float decode_float(uint8_t const *,int);
double decode_double(uint8_t const *,int);
struct sockaddr *decode_socket(void *,uint8_t const *,int);
struct sockaddr *decode_local_socket(void *,uint8_t const *,int);
char *decode_string(uint8_t const *,int);
uint32_t get_ssrc(uint8_t const *buffer,int length);
uint32_t get_tag(uint8_t const *buffer,int length);

void dump_metadata(FILE *,uint8_t const *,int,bool);

#endif
