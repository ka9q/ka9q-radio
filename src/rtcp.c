#include <unistd.h>
#include <sys/types.h>
#include <pwd.h>
#include <uuid/uuid.h>
#include "radio.h"

// RTP control protocol sender task
// this thread is joined during shutdown with the channel lock held, so this cannot hold it
void *rtcp_send(void *arg){
  chan_t *chan = (chan_t *)arg;
  if(chan == NULL)
    pthread_exit(NULL);

  {
    char name[100];
    snprintf(name,sizeof(name),"rtcp %u",chan->output.rtp.ssrc);
    pthread_setname(name);
  }

  int64_t Starttime = gps_time_ns();      // System clock at timestamp 0, for RTCP
  while(true){

    if(chan->output.rtp.ssrc == 0) // Wait until it's set by output RTP subsystem
      goto done;
    uint8_t buffer[PKTSIZE]; // much larger than necessary
    memset(buffer,0,sizeof(buffer));

    // Construct sender report
    struct rtcp_sr sr;
    memset(&sr,0,sizeof(sr));
    sr.ssrc = chan->output.rtp.ssrc;

    // Construct NTP timestamp (NTP uses UTC, ignores leap seconds)
    {
      struct timespec now;
      clock_gettime(CLOCK_REALTIME,&now);
      sr.ntp_timestamp = ((int64_t)now.tv_sec + NTP_EPOCH) << 32;
      sr.ntp_timestamp += ((int64_t)now.tv_nsec << 32) / BILLION; // NTP timestamps are units of 2^-32 sec
    }
    // The zero is to remind me that I start timestamps at zero, but they could start anywhere
    sr.rtp_timestamp = (unsigned)((0 + gps_time_ns() - Starttime) / BILLION);
    sr.packet_count = chan->output.rtp.seq;
    sr.byte_count = (unsigned)chan->output.rtp.bytes;

    uint8_t *dp = gen_sr(buffer,sizeof(buffer),&sr,NULL,0);

    // Construct SDES
    struct rtcp_sdes sdes[4];

    // CNAME
    char *string = NULL;
    int sl = asprintf(&string,"radio@%s",Hostname);
    if(sl > 0 && sl <= 255){
      sdes[0].type = CNAME;
      strlcpy(sdes[0].message,string,sizeof(sdes[0].message));
      sdes[0].mlen = strlen(sdes[0].message);
    }
    FREE(string);

    sdes[1].type = NAME;
    strlcpy(sdes[1].message,"KA9Q Radio Program",sizeof(sdes[1].message));
    sdes[1].mlen = strlen(sdes[1].message);

    sdes[2].type = EMAIL;
    strlcpy(sdes[2].message,"karn@ka9q.net",sizeof(sdes[2].message));
    sdes[2].mlen = strlen(sdes[2].message);

    sdes[3].type = TOOL;
    strlcpy(sdes[3].message,"KA9Q Radio Program",sizeof(sdes[3].message));
    sdes[3].mlen = strlen(sdes[3].message);

    dp = gen_sdes(dp,sizeof(buffer) - (dp-buffer),chan->output.rtp.ssrc,sdes,4);

    socklen_t const slen = chan->rtcp.dest_socket.ss_family == AF_INET ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6);
    if(sendto(Output_fd,buffer,dp-buffer,0,(struct sockaddr *)&chan->rtcp.dest_socket,slen) < 0)
      chan->output.errors++;
  done:;
    sleep(1);
  }
}


/* Session announcement protocol - highly experimental, off by default
   The whole point was to make it easy to use VLC and similar tools, but they either don't actually implement SAP (e.g. in iOS)
   or implement some vague subset that you have to guess how to use
   Will probably work better with Opus streams from the opus transcoder, since they're always 48000 Hz stereo; no switching midstream
*/
void *sap_send(void *p){
  chan_t *chan = (chan_t *)p;
  assert(chan != NULL);
  if(chan == NULL)
    return NULL;

  int64_t start_time = utc_time_sec() + NTP_EPOCH; // NTP uses UTC, not GPS

  // These should change when a change is made elsewhere
  uint16_t const id = (uint16_t)random(); // Should be a hash, but it changes every time anyway
  int const sess_version = 1;

  for(;;){
    char message[PKTSIZE],*wp;
    int space = sizeof(message);
    wp = message;

    *wp++ = 0x20; // SAP version 1, ipv4 address, announce, not encrypted, not compressed
    *wp++ = 0; // No authentication
    *wp++ = id >> 8;
    *wp++ = id & 0xff;
    space -= 4;

    // our sending ipv4 address
    struct sockaddr_in const *sin = (struct sockaddr_in *)&chan->output.source_socket;
    uint32_t *src = (uint32_t *)wp;
    *src = sin->sin_addr.s_addr; // network byte order
    wp += 4;
    space -= 4;

    int len = snprintf(wp,space,"application/sdp");
    wp += len + 1; // allow space for the trailing null
    space -= (len + 1);

    // End of SAP header, beginning of SDP

    // Version v=0 (always)
    len = snprintf(wp,space,"v=0\r\n");
    wp += len;
    space -= len;

    {
      // Originator o=
      char hostname[sysconf(_SC_HOST_NAME_MAX)];
      gethostname(hostname,sizeof(hostname));

      struct passwd pwd,*result = NULL;
      char buf[1024];

      getpwuid_r(getuid(),&pwd,buf,sizeof(buf),&result);
      len = snprintf(wp,space,"o=%s %lld %d IN IP4 %s\r\n",
		     result ? result->pw_name : "-",
		     (long long)start_time,sess_version,hostname);

      wp += len;
      space -= len;
    }

    // s= (session name)
    len = snprintf(wp,space,"s=radio %s\r\n",Frontend.description);
    wp += len;
    space -= len;

    // i= (human-readable session information)
    len = snprintf(wp,space,"i=PCM output stream from ka9q-radio on %s\r\n",Frontend.description);
    wp += len;
    space -= len;

    {
      char *mcast = strdup(formatsock(&chan->output.dest_socket,false));
      assert(mcast != NULL);
      // Remove :port field, confuses the vlc listener
      char *cp = strchr(mcast,':');
      if(cp)
	*cp = '\0';
      len = snprintf(wp,space,"c=IN IP4 %s/%d\r\n",mcast,chan->output.ttl);
      wp += len;
      space -= len;
      FREE(mcast);
    }


#if 0 // not currently used
    int64_t current_time = utc_time_sec() + NTP_EPOCH;
#endif

    // t= (time description)
    len = snprintf(wp,space,"t=%lld %lld\r\n",(long long)start_time,0LL); // unbounded
    wp += len;
    space -= len;

    // m = media description
    // set from current state. This will require changing the session version and IDs, and
    // it's not clear that clients like VLC will do the right thing anyway
    len = snprintf(wp,space,"m=audio 5004/1 RTP/AVP %d\r\n",chan->output.rtp.type);
    wp += len;
    space -= len;

    len = snprintf(wp,space,"a=rtpmap:%d %s/%d/%d\r\n",
		   chan->output.rtp.type,
		   encoding_string(chan->output.encoding),
		   chan->output.samprate,
		   chan->output.channels);
    wp += len;
    space -= len;

    int const outsock = chan->output.ttl != 0 ? Output_fd : Output_fd0;
    socklen_t const slen = chan->sap.dest_socket.ss_family == AF_INET ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6);
    if(sendto(outsock,message,wp - message,0,(struct sockaddr *)&chan->sap.dest_socket,
	      slen) < 0)
      chan->output.errors++;
    sleep(5);
  }
}


// Real Time Control Protocol (RTCP) for ka9q-radio - very primitive and not yet complete or tested
// Sep 2018-2023 Phil Karn, KA9Q
#include <sys/types.h>
#include <string.h>
#include <stdint.h>
#include "multicast.h"
#include "rtp.h"

// Build a RTCP sender report in network order
// Return pointer to byte after end of written packet
uint8_t *gen_sr(uint8_t *output,size_t bufsize,struct rtcp_sr const *sr,struct rtcp_rr const *rr,int rc){

  int words = 1 + 6 + 6*rc;

  if(4*words > (int)bufsize)
    return NULL; // Not enough room in buffer

  // SR packet header
  *output++ = (uint8_t)((2 << 6) | rc);
  *output++ = 200;
  output = put16(output,words-1);

  // Sender info
  output = put32(output,sr->ssrc);
  output = put32(output,(int32_t)(sr->ntp_timestamp >> 32));
  output = put32(output,(int32_t)sr->ntp_timestamp);
  output = put32(output,sr->rtp_timestamp);
  output = put32(output,sr->packet_count);
  output = put32(output,sr->byte_count);

  // Receiver info (if any)
  for(int i=0; i < rc; i++){
    output = put32(output,rr->ssrc);
    *output++ = (uint8_t)rr->lost_fract;
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
uint8_t *gen_rr(uint8_t *output,size_t bufsize,uint32_t ssrc,struct rtcp_rr const *rr,int rc){

  int words = 2 + 6*rc;

  if(4*words > (int)bufsize)
    return NULL; // Not enough room in buffer

  // RR packet header
  *output++ = (uint8_t)((2 << 6) | rc);
  *output++ = 201; // Receiver report
  output = put16(output,words-1);
  output = put32(output,ssrc);

  // Receiver info (if any)
  for(int i=0; i < rc; i++){
    output = put32(output,rr->ssrc);
    *output++ = (uint8_t)rr->lost_fract;
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
uint8_t *gen_sdes(uint8_t *output,size_t bufsize,uint32_t ssrc,struct rtcp_sdes const *sdes,int sc){
  
  if(sc < 0 || sc > 31) // Range check on source count
    return NULL;

  // Calculate size
  int bytes = 4 + 4 + 1; // header + ssrc + terminating null
  for(int i=0; i < sc; i++){
    if(sdes[i].mlen > 255)
      return NULL;
    bytes += 2 + sdes[i].mlen; // type + length + item
  }
  // Round up to 4 byte boundary
  int words = (bytes + 3)/4;

  if(4*words > (int)bufsize)
    return NULL;

  memset(output,0,bufsize); // easist way to guarantee nulls at end
  uint8_t *dp = output;

  *dp++ = (2 << 6) | 1; // Only one chunk per message at present
  *dp++ = 202; // SDES
  dp = put16(dp,words-1);
  dp = put32(dp,ssrc);

  // Put each item
  for(int i=0; i<sc; i++){
    *dp++ = (uint8_t)sdes[i].type;
    *dp++ = (uint8_t)sdes[i].mlen;
    memcpy(dp,sdes[i].message,sdes[i].mlen); // Buffer overrun avoided by size calc?
    dp += sdes[i].mlen;
  }
  return output + words*4;
}

uint8_t *gen_bye(uint8_t *output,size_t bufsize,uint32_t const *ssrcs,int sc){
  if(sc < 0 || sc > 31) // Range check on source count
    return NULL;

  int words = 1 + sc;
  if(4*words > (int)bufsize)
    return NULL;

  *output++ = (uint8_t)((2 << 6) | sc);
  *output++ = 203; // BYE
  output = put16(output,words-1);

  for(int i=0; i<sc; i++)
    output = put32(output,ssrcs[i]);

  return output;
}
