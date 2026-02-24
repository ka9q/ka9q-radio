// Audio multicast routines for ka9q-radio
// Handles linear 16-bit PCM, mono and stereo
// Copyright 2017-2024 Phil Karn, KA9Q

#define _GNU_SOURCE 1
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <limits.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <stdatomic.h>
#include <opus/opus.h>

#include "misc.h"
#include "multicast.h"
#include "radio.h"
#include "import.h"

// byte count to fit in Ethernet MTU
// For lower sample rates this won't matter at all
// At much higher rates it will minimize the total packets sent every 20 ms
#define BYTES_PER_PKT 1440 // 3 frames of 16-bit LPCM @ 12 kHz, a common value

bool GetSockOptFailed = false;     // Have we issued this log message yet?
bool TempSendFailure = false;

int Fec_percent = 0;               // Use forward error correction percentage, 0-100

static atomic_flag Opus_version_logged = ATOMIC_FLAG_INIT;

static int setup_opus(struct channel *chan);
static int max_frames(struct channel *chan);

// Send PCM output on stream; # of channels implicit in chan->output.channels
int send_output(struct channel * restrict const chan, float const * buffer, int frames, bool const mute){
  assert(chan != NULL);
  if(frames <= 0 || chan->output.channels == 0 || chan->output.samprate == 0)
    return 0;

  if(mute || buffer == NULL){
    // Still increment timestamp
    if(chan->output.encoding == OPUS || chan->output.encoding == OPUS_VOIP)
      chan->output.rtp.timestamp += frames * OPUS_SAMPRATE / chan->output.samprate; // Opus always at 48 kHz
    else
      chan->output.rtp.timestamp += frames;

    chan->output.silent = true;
    return 0;
  }
  if((chan->output.encoding == OPUS || chan->output.encoding == OPUS_VOIP) && setup_opus(chan) != 0)
    return 0;

  int max_frames_per_pkt = max_frames(chan); // depends on coding

  useconds_t pacing = chan->output.pacing ? 1000 : 0; // fix it at a millisecond for now

  int frames_sent = 0;
  int available_frames = chan->output.queue_length + frames;

  while(available_frames >= max_frames_per_pkt
	|| (available_frames > 0 && chan->output.queue_age >= chan->output.maxdelay)){
    // We have enough data to send at least one full size packet OR we've run out of time and there's something to send
    struct rtp_header rtp = {
      .version = RTP_VERS,
      .ssrc = chan->output.rtp.ssrc,
      .timestamp = chan->output.rtp.timestamp,
      .seq = chan->output.rtp.seq,
      .type = chan->output.rtp.type,
      .marker = chan->output.silent
    };
    chan->output.silent = false;

    uint8_t packet[PKTSIZE];
    uint8_t * const dp = (uint8_t *)hton_rtp(packet,&rtp); // First byte after RTP header to be written
    int chunk = frames;
    float const *buf = buffer;

    if(chan->output.queue_length > 0){
      // There's something in the buffer, send it first
      if(chan->output.queue_length < max_frames_per_pkt){
	// Try to fill it out with new data if available
	int copylen = max_frames_per_pkt - chan->output.queue_length;
	if(copylen > frames)
	  copylen = frames; // limit to what we have
	chan->output.queue = realloc(chan->output.queue, (chan->output.queue_length + copylen) * chan->output.channels * sizeof(float));
	memcpy(chan->output.queue + chan->output.channels * chan->output.queue_length,
	       buffer, copylen * chan->output.channels * sizeof(float));
	chan->output.queue_length += copylen;
	frames -= copylen;
	buffer += copylen * chan->output.channels;
      }
      buf = chan->output.queue;
      chunk = chan->output.queue_length; // we will try to send it all, shouldn't exceed max_frames_per_pkt
    }
    if(chunk > max_frames_per_pkt)
      chunk = max_frames_per_pkt;

    uint8_t *ndp = NULL; // pointer to first unwritten byte after export call
    int const samples = chunk * chan->output.channels;
    switch(chan->output.encoding){
    case MULAW:
      ndp = export_mulaw(dp,buf,samples);
      break;
    case ALAW:
      ndp = export_alaw(dp,buf,samples);
      break;
    case S16BE:
      ndp = export_s16_be(dp,buf,samples);
      break;
    case S16LE:
      ndp = export_s16_le(dp,buf,samples);
      break;
    case F32BE:
      ndp = export_f32_be(dp,buf,samples);
      break;
    case F32LE:
      ndp = export_f32_le(dp,buf,samples);
      break;
#ifdef HAS_FLOAT16
    case F16LE:
      ndp = export_f16_le(dp,buf,samples);
      break;
    case F16BE:
      ndp = export_f16_be(dp,buf,samples);
      break;
#endif
    case OPUS_VOIP:
    case OPUS:
      {
	// Enforce supported Opus packet sizes
	size_t si;
	for(si = 0; Opus_blocksizes[si] > 0; si++){
	  if(10000 * chunk < Opus_blocksizes[si] * chan->output.samprate)
	    break;
	}
	if(si == 0)
	  goto quit;	// too small for the smallest frame

	chunk = Opus_blocksizes[si-1] * chan->output.samprate / 10000;
	// Opus says max possible packet size (on high fidelity audio) is 1275 bytes at 20 ms, which fits Ethernet
	// But this could conceivably fragment
	assert(dp <= packet + sizeof packet);
	opus_int32 const room = (opus_int32)(packet + sizeof packet - dp); // Max # bytes in compressed output buffer
	int const r = opus_encode_float(chan->opus.encoder, buf, chunk, dp, room); // Max # bytes in compressed output buffer
	if(r < 0)
	  fprintf(stderr,"opus encode %d bytes fail %d: %s\n", chunk, r, opus_strerror(r));
	else
	  ndp = dp + r;
	assert(r >= 0);
      }
      break;
    default: // probably can't get here because max_frames() returns 0
      chan->output.silent = true;
      break;
    }
    if(ndp == NULL)
      break; // No valid encoding
    int const bytes = ndp - packet;

    if(chan->output.encoding == OPUS || chan->output.encoding == OPUS_VOIP)
      chan->output.rtp.timestamp += chunk * OPUS_SAMPRATE / chan->output.samprate; // Always increases at 48 kHz
    else
      chan->output.rtp.timestamp += chunk;

    if(chan->output.queue_length > 0){
      // We just consumed buffered data
      // Might not be all gone if Opus reduced the chunk
      chan->output.queue_age = 0;
      chan->output.queue_length -= chunk;
      assert(chan->output.queue_length >= 0);
      if(chan->output.queue_length > 0)
	memmove(chan->output.queue,
		chan->output.queue + chunk * chan->output.channels,
		chan->output.queue_length * chan->output.channels * sizeof(float));
    } else {
      // consumed from new data
      buffer += chunk * chan->output.channels;
      frames -= chunk;
    }
    chan->output.samples += chunk; // Count stereo frames
    available_frames -= chunk;
    frames_sent += chunk;

    if(bytes > 0){ // Suppress Opus DTX frames (bytes == 0)
      int const outsock = chan->output.ttl != 0 ? Output_fd : Output_fd0;
      ssize_t const r = sendto(outsock, &packet, bytes, 0, (struct sockaddr *)&chan->output.dest_socket, sizeof chan->output.dest_socket);
      chan->output.rtp.bytes += bytes;
      chan->output.rtp.packets++;
      chan->output.rtp.seq++;
      if(r < 0){
	chan->output.errors++;
	if(errno == EAGAIN){
	  if(!TempSendFailure){
	    fprintf(stderr,"Temporary send failure, suggest increased buffering (see sysctl net.core.wmem_max, net.core.wmem_default\n");
	    fprintf(stderr,"Additional messages suppressed\n");
	    TempSendFailure = true;
	  }
	} else {
	  fprintf(stderr,"audio send failure: %s\n",strerror(errno));
	  abort(); // Probably more serious, like the loss of an interface or route; restart from systemd
	}
      }
      if(chan->output.pacing && available_frames > 0)
	usleep(pacing);
    }
  }
 quit:
  // Any left that we must buffer?
  if(frames > 0){
    chan->output.queue = realloc(chan->output.queue, (chan->output.queue_length + frames) * chan->output.channels * sizeof(float));
    memcpy(chan->output.queue + chan->output.channels * chan->output.queue_length,
	   buffer,
	   frames * chan->output.channels * sizeof(float));
    chan->output.queue_length += frames;
  }
  if(chan->output.queue_length > 0)
    chan->output.queue_age++; // Timer runs whenever there's anything pending

  return frames_sent;
}

static int setup_opus(struct channel *chan){
  if(chan->opus.encoder != NULL){
    // There doesn't seem to be any way to read back the channel count, so we save that explicitly
    // If the sample rate changes we'll get restarted anyway, so this test isn't really needed. But do it anyway.
    opus_int32 s;
    opus_int32 ret = opus_encoder_ctl(chan->opus.encoder,OPUS_GET_SAMPLE_RATE(&s));
    if(ret != OPUS_OK || s != chan->output.samprate || chan->opus.channels != chan->output.channels){
      opus_encoder_destroy(chan->opus.encoder);
      chan->opus.encoder = NULL;
      chan->opus.channels = 0;
    } else if(chan->output.silent) {
      // Reset existing encoder after silence period (flag must not have been cleared yet)
      opus_encoder_ctl(chan->opus.encoder,OPUS_RESET_STATE);
    }
  }
  if(chan->opus.encoder == NULL){
    int error = OPUS_OK;
    // Opus only supports a specific set of sample rates
    if(!legal_opus_samprate(chan->output.samprate)){
      // Simply drop until somebody fixes it
      chan->output.silent = true;
      return -1;
    }
    if(!atomic_flag_test_and_set_explicit(&Opus_version_logged,memory_order_relaxed)){
      // Log this message only once
      fprintf(stderr,"Using %s\n",opus_get_version_string());
    }
    chan->opus.encoder = opus_encoder_create(chan->output.samprate,chan->output.channels,chan->opus.application,&error);
    assert(error == OPUS_OK && chan->opus.encoder != NULL);
    chan->opus.channels = chan->output.channels; // In case it changes
    chan->opus.bandwidth = -1; // force it to be set the first time
  }
  // Dynamically set or change various Opus options
  /* Set the bit depth according to the actual SNR, which is unlikely to be high
     Effect on actual bit rate is hard to determine
     The allowed range is 8-24 bits but few linear channels are even as good as 8 bits (~48 dB SNR)
     We are using float samples so the SNR could theoretically be > 100 dB, but 16 bits seems good enough
     since we would otherwise be emitting 16-bit PCM
  */
  int opus_bits = 16;
  if(chan->demod_type == LINEAR_DEMOD) {
    double const noise_bandwidth = fabs(chan->filter.max_IF - chan->filter.min_IF);
    double sig_power = chan->sig.bb_power - noise_bandwidth * chan->sig.n0;
    sig_power = max(sig_power,0.0); // Avoid log(-x) = nan
    double const sn0 = chan->sig.n0 == 0 ? INFINITY : sig_power/chan->sig.n0;
    double const snr = power2dB(sn0/noise_bandwidth);
    if(snr < 48)
      opus_bits = 8; // Use a floor of 8 bit precision, which is usually the case for comm quality channels
    else if(snr > 100)
      opus_bits = 16;
    else
      opus_bits = lrint(snr / 6); // 6 dB SNR per bit; don't operate on -infinite SNRs
  }
  int error = opus_encoder_ctl(chan->opus.encoder,OPUS_SET_LSB_DEPTH(opus_bits));
  if(error != OPUS_OK)
    fprintf(stderr,"set bit depth error %d: %s\n",error,opus_strerror(error));
  assert(error == OPUS_OK);
  // Set the encoder bandwidth automatically according to the filter bandwidth
  // Questionable how much this helps, but it doesn't seem to hurt
  int opus_bw_code = OPUS_BANDWIDTH_FULLBAND;
  switch(chan->demod_type){
  case FM_DEMOD:
    // NBFM uses 24 ks/s to handle the 16 kHz IF bandwidth; the baseband bandwidth is really only 5 kHz
    // NBFM should probably use a sample rate converter to save output bits
    if(chan->output.samprate <= 24000)
      opus_bw_code = OPUS_BANDWIDTH_MEDIUMBAND;
    break;
  case LINEAR_DEMOD:
    {
      // Set opus bandwidth according to IF filter
      double filter_bandwidth;
      if(chan->filter2.blocking > 0)
	filter_bandwidth = max(fabs(chan->filter2.low),fabs(chan->filter2.high));
      else
	filter_bandwidth = max(fabs(chan->filter.min_IF),fabs(chan->filter.max_IF));
      opus_bw_code = opus_bandwidth_to_code(filter_bandwidth);
    }
    break;
  default: // Just use fullband for WFM
    break;
  }
  error = opus_encoder_ctl(chan->opus.encoder,OPUS_SET_MAX_BANDWIDTH(opus_bw_code));
  chan->opus.bandwidth = opus_bw_code;
  if(error != OPUS_OK)
    fprintf(stderr,"set max bandwidth %d error %d: %s\n",opus_bw_code,error,opus_strerror(error));
  assert(error == OPUS_OK);

  error = opus_encoder_ctl(chan->opus.encoder,OPUS_SET_DTX(chan->opus.dtx));
  if(error != OPUS_OK)
    fprintf(stderr,"set dtx %d error %d: %s\n",chan->opus.dtx,error,opus_strerror(error));
  assert(error == OPUS_OK);

  error = opus_encoder_ctl(chan->opus.encoder,OPUS_SET_BITRATE(chan->opus.bitrate == 0 ? OPUS_AUTO : chan->opus.bitrate));
  if(error != OPUS_OK)
    fprintf(stderr,"set bitrate %d error %d: %s\n",chan->opus.bitrate,error,opus_strerror(error));
  assert(error == OPUS_OK);

  error = opus_encoder_ctl(chan->opus.encoder,OPUS_SET_INBAND_FEC(chan->opus.fec > 0 ? 1 : 0));
  if(error != OPUS_OK)
    fprintf(stderr,"set inband fec %d error %d: %s\n",chan->opus.fec,error,opus_strerror(error));
  assert(error == OPUS_OK);

  error = opus_encoder_ctl(chan->opus.encoder,OPUS_SET_PACKET_LOSS_PERC(chan->opus.fec));
  if(error != OPUS_OK)
    fprintf(stderr,"set packet loss %d%% error %d: %s\n",chan->opus.fec,error,opus_strerror(error));
  assert(error == OPUS_OK);

  opus_int32 signal = 0;
  error = opus_encoder_ctl(chan->opus.encoder,OPUS_GET_SIGNAL(&signal));
  if(error != OPUS_OK)
    fprintf(stderr,"get signal error %d: %s\n",error,opus_strerror(error));
  assert(error == OPUS_OK);
  if(signal != chan->opus.signal){
    error = opus_encoder_ctl(chan->opus.encoder,OPUS_SET_SIGNAL(chan->opus.signal));
    if(error != OPUS_OK)
      fprintf(stderr,"set signal %d error %d: %s\n",chan->opus.signal,error,opus_strerror(error));
    assert(error == OPUS_OK);

    error = opus_encoder_ctl(chan->opus.encoder,OPUS_SET_VBR_CONSTRAINT(0));
    if(error != OPUS_OK)
      fprintf(stderr,"set vbr constraint error %d: %s\n",error,opus_strerror(error));

    assert(error == OPUS_OK);
  }
  return 0;
}
static int max_frames(struct channel *chan){
  // The PCM modes are limited by the Ethenet MTU
  // Opus is essentially unlimited as it should never fill an ethernet (?)
  int max_frames_per_pkt = 0;
  switch(chan->output.encoding){
  default: // Just drop
    return 0;
  case S16BE:
  case S16LE:
    max_frames_per_pkt = BYTES_PER_PKT / (sizeof(int16_t) * chan->output.channels);
    break;
  case F32LE:
  case F32BE:
    max_frames_per_pkt = BYTES_PER_PKT / (sizeof(float) * chan->output.channels);
    break;
#ifdef HAS_FLOAT16
  case F16LE:
  case F16BE:
    max_frames_per_pkt = BYTES_PER_PKT / (sizeof(float16_t) * chan->output.channels);
    break;
#endif
  case OPUS:
    max_frames_per_pkt = floor(chan->output.samprate * 0.12); // 120 ms is biggest Opus frame
    break;
  case MULAW:
  case ALAW:
    max_frames_per_pkt = BYTES_PER_PKT / (sizeof(uint8_t) * chan->output.channels);
    break;
  }
  return max_frames_per_pkt;
}
