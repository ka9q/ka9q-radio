// $Id: rtcp.c,v 1.2 2018/09/05 08:18:22 karn Exp $
// Real Time Control Protocol (RTCP)
// Sep 2018 Phil Karn, KA9Q
#include <sys/types.h>
#include <string.h>
#include "multicast.h"

// Build a RTCP sender report in network order
// Return pointer to byte after end of written packet
unsigned char *gen_sr(unsigned char *output,int bufsize,struct rtcp_sr const *sr,struct rtcp_rr const *rr,int rc){

  int words = 1 + 6 + 6*rc;

  if(4*words > bufsize)
    return NULL; // Not enough room in buffer

  // SR packet header
  *output++ = (2 << 6) | rc;
  *output++ = 200;
  output = put16(output,words-1);

  // Sender info
  output = put32(output,sr->ssrc);
  output = put32(output,sr->ntp_timestamp >> 32);
  output = put32(output,sr->ntp_timestamp);
  output = put32(output,sr->rtp_timestamp);
  output = put32(output,sr->packet_count);
  output = put32(output,sr->byte_count);

  // Receiver info (if any)
  for(int i=0; i < rc; i++){
    output = put32(output,rr->ssrc);
    *output++ = rr->lost_fract;
    output = put24(output,rr->lost_packets);
    output = put32(output,rr->highest_seq);
    output = put32(output,rr->jitter);
    output = put32(output,rr->lsr);
    output = put32(output,rr->dlsr);
    rr++;
  }
  return output;
}
// Build a RTCP receiver report in network order
// Return pointer to byte after end of written packet
unsigned char *gen_rr(unsigned char *output,int bufsize,uint32_t ssrc,struct rtcp_rr const *rr,int rc){

  int words = 2 + 6*rc;

  if(4*words > bufsize)
    return NULL; // Not enough room in buffer

  // RR packet header
  *output++ = (2 << 6) | rc;
  *output++ = 201; // Receiver report
  output = put16(output,words-1);
  output = put32(output,ssrc);

  // Receiver info (if any)
  for(int i=0; i < rc; i++){
    output = put32(output,rr->ssrc);
    *output++ = rr->lost_fract;
    output = put24(output,rr->lost_packets);
    output = put32(output,rr->highest_seq);
    output = put32(output,rr->jitter);
    output = put32(output,rr->lsr);
    output = put32(output,rr->dlsr);
    rr++;
  }
  return output;
}


// Build a RTCP source description packet in network order
// Return pointer to byte after end of written packet
unsigned char *gen_sdes(unsigned char *output,int bufsize,uint32_t ssrc,struct rtcp_sdes const *sdes,int sc){
  
  if(sc < 0 || sc > 31) // Range check on source count
    return NULL;

  // Calculate size
  int bytes = 4 + 4 + 1; // header + ssrc + terminating null
  for(int i=0; i < sc; i++){
    if(sdes[i].mlen < 0 || sdes[i].mlen > 255)
      return NULL;
    bytes += 2 + sdes[i].mlen; // type + length + item
  }
  // Round up to 4 byte boundary
  int words = (bytes + 3)/4;

  if(4*words > bufsize)
    return NULL;

  memset(output,0,bufsize); // easist way to guarantee nulls at end
  unsigned char *dp = output;

  *dp++ = (2 << 6) | 1; // Only one chunk per message at present
  *dp++ = 202; // SDES
  dp = put16(dp,words-1);
  dp = put32(dp,ssrc);

  // Put each item
  for(int i=0; i<sc; i++){
    *dp++ = sdes[i].type;
    *dp++ = sdes[i].mlen;
    memcpy(dp,sdes[i].message,sdes[i].mlen); // Buffer overrun avoided by size calc?
    dp += sdes[i].mlen;
  }
  return output + words*4;
}

unsigned char *gen_bye(unsigned char *output,int bufsize,uint32_t const *ssrcs,int sc){
  if(sc < 0 || sc > 31) // Range check on source count
    return NULL;

  int words = 1 + sc;
  if(4*words > bufsize)
    return NULL;

  *output++ = (2 << 6) | sc;
  *output++ = 203; // BYE
  output = put16(output,words-1);

  for(int i=0; i<sc; i++)
    output = put32(output,ssrcs[i]);

  return output;
}
