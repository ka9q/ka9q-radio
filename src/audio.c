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


// byte count to fit in Ethernet MTU
// For lower sample rates this won't matter at all
// At much higher rates it will minimize the total packets sent every 20 ms
#define BYTES_PER_PKT 1440 // 3 frames of 16-bit LPCM @ 12 kHz, a common value

bool GetSockOptFailed = false;     // Have we issued this log message yet?
bool TempSendFailure = false;

int Fec_percent = 0;               // Use forward error correction percentage, 0-100

static atomic_flag Opus_version_logged = ATOMIC_FLAG_INIT;


// Send PCM output on stream; # of channels implicit in chan->output.channels
int send_output(struct channel * restrict const chan,float const * restrict buffer,int frames,bool const mute){
  assert(chan != NULL);
  if(frames <= 0 || chan->output.channels == 0 || chan->output.samprate == 0)
    return 0;

  if(mute){
    flush_output(chan,false,true);

    // Still increment timestamp
    if(chan->output.encoding == OPUS)
      chan->output.rtp.timestamp += frames * 48000 / chan->output.samprate; // Opus always at 48 kHz
    else
      chan->output.rtp.timestamp += frames;

    chan->output.silent = true;
    return 0;
  }
  bool marker = false;
  // Send a marker to reset the receiver when the stream restarts
  if(chan->output.silent){
    marker = true;
    int count = flush_output(chan,marker,true);
    if(count != 0){
      // A mark has been sent, don't need to send it again
      marker = false;
      chan->output.silent = false;
    }
  }
  size_t needed_size = frames * chan->output.channels * (1 + chan->output.minpacket);
  if(chan->output.queue == NULL || (needed_size > 0 && needed_size > chan->output.queue_size)){
    // Enlarge the output queue
    flush_output(chan,marker,true); // if still set, marker won't get sent since it wasn't sent last time; no-op if empty/null
    mirror_free((void *)&chan->output.queue,chan->output.queue_size * sizeof(float)); // no-op if null
    size_t const size = round_to_page(sizeof(float) * needed_size); // mmap requires even number of pages
    chan->output.queue = mirror_alloc(size);
    chan->output.queue_size = size/ sizeof(float);
    chan->output.rp = chan->output.wp = 0;
  }

  // Copy into queue
  memcpy(&chan->output.queue[chan->output.wp],buffer,sizeof(float) * frames * chan->output.channels);
  chan->output.wp += frames * chan->output.channels; // Number of floats written
  // handle wrap
  if(chan->output.wp >= chan->output.queue_size)
    chan->output.wp -= chan->output.queue_size;
  int count = flush_output(chan,marker,false);  // Send only full size packets
  if(count != 0){
    // A mark has been sent, don't need to send it again
    marker = false;
    chan->output.silent = false;
  }
  return frames; // Number of frames enqueued
}
// Flush the output queue
// if marker == true, set mark in first (only) RTP packet
// If complete == true, send everything
//    complete == false, send only full-size packets
// Opus will always flush into a single packet
int flush_output(struct channel * chan,bool marker,bool complete){
  if(chan == NULL)
    return -1;
  if(chan->output.queue == NULL || chan->output.rp == chan->output.wp)
    return 0; // Nothing to send; will happen on initial flush

  // When flushing, anything will do
  int min_frames_per_pkt = 1;
  if(!complete && !marker && chan->output.minpacket > 0)
    min_frames_per_pkt = (int)round(chan->output.minpacket * Blocktime * chan->output.samprate);

  // The PCM modes are limited by the Ethenet MTU
  // Opus is essentially unlimited as it should never fill an ethernet (?)
  int max_frames_per_pkt = 0;
  switch(chan->output.encoding){
  default: // Just drop
    chan->output.rp = chan->output.wp;
    return 0;
  case S16BE:
  case S16LE:
    max_frames_per_pkt = BYTES_PER_PKT / (sizeof(int16_t) * chan->output.channels);
    break;
  case F32LE:
    max_frames_per_pkt = BYTES_PER_PKT / (sizeof(float) * chan->output.channels);
    break;
#ifdef HAS_FLOAT16
  case F16LE:
    max_frames_per_pkt = BYTES_PER_PKT / (sizeof(float16_t) * chan->output.channels);
    break;
#endif
  case OPUS:
    max_frames_per_pkt = INT_MAX; // No real limit
    break;
  }
  if(min_frames_per_pkt > max_frames_per_pkt)
    min_frames_per_pkt = max_frames_per_pkt;

  useconds_t pacing = 0;
  if(chan->output.pacing)
    pacing = 1000; // fix it at a millisecond for now

  if(chan->output.encoding == OPUS){
    if(chan->opus.encoder != NULL){
      // There doesn't seem to be any way to read back the channel count, so we save that explicitly
      // If the sample rate changes we'll get restarted anyway, so this test isn't really needed. But do it anyway.
      opus_int32 s;
      opus_int32 ret = opus_encoder_ctl(chan->opus.encoder,OPUS_GET_SAMPLE_RATE(&s));
      if(ret != OPUS_OK || s != chan->output.samprate || chan->opus.channels != chan->output.channels){
	opus_encoder_destroy(chan->opus.encoder);
	chan->opus.encoder = NULL;
	chan->opus.channels = 0;
      } else if(marker) {
	// Reset existing encoder after silence period
	opus_encoder_ctl(chan->opus.encoder,OPUS_RESET_STATE);
      }
    }
    int error = OPUS_OK;
    if(chan->opus.encoder == NULL){
      // Opus only supports a specific set of sample rates
      if(!legal_opus_samprate(chan->output.samprate)){
	// Simply drop until somebody fixes it
	chan->output.silent = true;
	chan->output.rp = chan->output.wp;
	return 0;
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
      double const sn0 = sig_power/chan->sig.n0;
      double const snr = power2dB(sn0/noise_bandwidth);
      opus_bits = (int)round(snr / 6); // 6 dB SNR per bit
      opus_bits = max(opus_bits,8);  // don't go to ridiculous values on no signal
      opus_bits = min(opus_bits,16); // Opus can actually take 24
      error = opus_encoder_ctl(chan->opus.encoder,OPUS_SET_LSB_DEPTH(opus_bits));
      if(error != OPUS_OK)
	fprintf(stderr,"set bit depth error %d\n",error);
      assert(error == OPUS_OK);
    }
    error = opus_encoder_ctl(chan->opus.encoder,OPUS_SET_LSB_DEPTH(opus_bits));
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
    assert(error == OPUS_OK);

    error = opus_encoder_ctl(chan->opus.encoder,OPUS_SET_DTX(chan->opus.dtx));
    assert(error == OPUS_OK);

    error = opus_encoder_ctl(chan->opus.encoder,OPUS_SET_BITRATE(chan->opus.bitrate == 0 ? OPUS_AUTO : chan->opus.bitrate));
    assert(error == OPUS_OK);

    error = opus_encoder_ctl(chan->opus.encoder,OPUS_SET_INBAND_FEC(chan->opus.fec > 0 ? 1 : 0));
    assert(error == OPUS_OK);

    error = opus_encoder_ctl(chan->opus.encoder,OPUS_SET_PACKET_LOSS_PERC(chan->opus.fec));
    assert(error == OPUS_OK);

    opus_int32 signal;
    error = opus_encoder_ctl(chan->opus.encoder,OPUS_GET_SIGNAL(&signal));
    assert(error == OPUS_OK);
    if(signal != chan->opus.signal){
      error = opus_encoder_ctl(chan->opus.encoder,OPUS_SET_SIGNAL(chan->opus.signal));
      assert(error == OPUS_OK);
    }

  } // if(chan->output.encoding == OPUS){

  int available_samples = (int)(chan->output.wp - chan->output.rp);
  if(available_samples < 0)
    available_samples += chan->output.queue_size;

  struct rtp_header rtp;
  memset(&rtp,0,sizeof(rtp));
  rtp.version = RTP_VERS;
  rtp.type = chan->output.rtp.type;
  rtp.ssrc = chan->output.rtp.ssrc;

  int available_frames = available_samples / chan->output.channels;
  int frames_sent = 0;
  while(available_frames >= min_frames_per_pkt){
    int chunk = min(max_frames_per_pkt,available_frames);
    rtp.timestamp = chan->output.rtp.timestamp;
    rtp.seq = chan->output.rtp.seq;
    rtp.marker = marker;
    marker = false; // only send once
    uint8_t packet[PKTSIZE];
    uint8_t * const dp = (uint8_t *)hton_rtp(packet,&rtp); // First byte after RTP header to be written
    int bytes = 0;
    float const *buf = &chan->output.queue[chan->output.rp]; // Point to first sample to be output
    switch(chan->output.encoding){
    case S16BE:
      {
	int16_t *pcm_buf = (int16_t *)dp;
	for(int i=0; i < chunk * chan->output.channels; i++)
	  *pcm_buf++ = htons(scaleclip(buf[i])); // Byte swap

	chan->output.rtp.timestamp += chunk;
	bytes = chunk * chan->output.channels * sizeof(*pcm_buf);
      }
      break;
    case S16LE:
      {
	int16_t *pcm_buf = (int16_t *)dp;
	for(int i=0; i < chunk * chan->output.channels; i++)
	  *pcm_buf++ = scaleclip(buf[i]); // No byte swap

	chan->output.rtp.timestamp += chunk;
	bytes = chunk * chan->output.channels * sizeof(*pcm_buf);
      }
      break;
    case F32LE:
      // Could use sendmsg() to avoid copy here since there's no conversion, but this doesn't use much
      memcpy(dp,buf,chunk * chan->output.channels * sizeof(float));
      chan->output.rtp.timestamp += chunk;
      bytes = chunk * chan->output.channels * sizeof(float);
      break;
#ifdef HAS_FLOAT16
    case F16LE:
      {
	float16_t *pcm_buf = (float16_t *)dp;
	for(int i=0; i < chunk * chan->output.channels; i++)
	  *pcm_buf++ = (float16_t)buf[i];

	chan->output.rtp.timestamp += chunk;
	bytes = chunk * chan->output.channels * sizeof(*pcm_buf);
      }
      break;
#endif
    case OPUS:
      {
	// Enforce supported Opus packet sizes
	size_t si;
	for(si = 0; Opus_blocksizes[si] != -1; si++){
	  if(chunk < Opus_blocksizes[si] * chan->output.samprate / 10000)
	    break;
	}
	if(si == 0)
	  goto quit;	// too small for the smallest frame

	chunk = Opus_blocksizes[si-1] * chan->output.samprate / 10000;
	// Opus says max possible packet size (on high fidelity audio) is 1275 bytes at 20 ms, which fits Ethernet
	// But this could conceivably fragment
	opus_int32 const room = (opus_int32)(sizeof(packet) - (dp-packet)); // Max # bytes in compressed output buffer
	bytes = opus_encode_float(chan->opus.encoder,buf,chunk,dp,room); // Max # bytes in compressed output buffer
	assert(bytes >= 0);

	opus_int32 d;
	opus_encoder_ctl(chan->opus.encoder,OPUS_GET_IN_DTX(&d));
	if(d == 1)
	  bytes = 0; // Suppress frame, but still increment timestamp

	chan->output.rtp.timestamp += chunk * OPUS_SAMPRATE / chan->output.samprate; // Always increases at 48 kHz
      }
      break;
    default:
      chan->output.silent = true;
      break;
    }
    // Handle wrap of read pointer
    chan->output.rp += chunk * chan->output.channels;
    if(chan->output.rp >= chan->output.queue_size)
      chan->output.rp -= chan->output.queue_size;

    if(bytes > 0){ // Suppress Opus DTX frames (bytes == 0)
      int const outsock = chan->output.ttl != 0 ? Output_fd : Output_fd0;
      ssize_t const r = sendto(outsock,&packet,bytes + (dp - packet),0,(struct sockaddr *)&chan->output.dest_socket,sizeof(chan->output.dest_socket));
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
	  abort(); // Probably more serious, like the loss of an interface or route
	}
      }
    }
    chan->output.samples += chunk * chan->output.channels; // Count stereo frames

    available_frames -= chunk;
    frames_sent += chunk;
    if(chan->output.pacing && available_frames > 0)
      usleep(pacing);
  }
 quit:
  return frames_sent;
}
