// $Id: multicast.c,v 1.91 2022/10/25 01:57:52 karn Exp $
// Multicast socket and RTP utility routines
// Copyright 2018 Phil Karn, KA9Q
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <string.h>
#include <net/if.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <ifaddrs.h>
#include <fcntl.h>
#include <errno.h>

#if defined(linux)
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <bsd/string.h>
#endif

#ifdef __APPLE__
#include <net/if_dl.h>
#endif

#include "multicast.h"
#include "misc.h"

static int ipv4_join_group(int const fd,void const * const sock,char const * const iface);
static int ipv6_join_group(int const fd,void const * const sock,char const * const iface);
static void set_ipv4_options(int fd,int mcast_ttl,int tos);
static void set_ipv6_options(int const fd,int const mcast_ttl,int const tos);

// [samprate][channels][deemph]
// Not all combinations are supported or useful,
// e.g., wideband FM is always 48 kHz, FM is always mono
static int pt_table[5][2] = {
  {  PCM_MONO_8_PT, PCM_STEREO_8_PT  },
  {  PCM_MONO_12_PT, PCM_STEREO_12_PT },
  {  PCM_MONO_16_PT, PCM_STEREO_16_PT },
  {  PCM_MONO_24_PT, PCM_STEREO_24_PT },
  {  PCM_MONO_PT, PCM_STEREO_PT },
};


// This is a bit messy. Is there a better way?
char const *Default_mcast_iface;


// Set up multicast socket for input or output

// Target points to string in the form of "domain[:port][,iface]"
// If target and sock are both non-null, the target will be resolved and copied into the sock structure
// If sock is null, the results of resolving target will not be stored there
// If target is null and sock is non-null, the existing sock structure contents will be used

// when output = 1, connect to the multicast address so we can simply send() to it without specifying a destination
// when output = 0, bind to it so we'll accept incoming packets
// Add parameter 'offset' (normally 0) to port number; this will be 1 when sending RTCP messages
// (Can we set up a socket for both input and output??)
int setup_mcast(char const * const target,struct sockaddr *sock,int const output,int const ttl,int const tos,int const offset){
  if(target == NULL && sock == NULL)
    return -1; // At least one must be supplied

  if(sock == NULL){
    sock = alloca(sizeof(struct sockaddr_storage));
    memset(sock,0,sizeof(struct sockaddr_storage));
  }
  char iface[1024];
  iface[0] = '\0';
  if(target)
    resolve_mcast(target,sock,DEFAULT_RTP_PORT+offset,iface,sizeof(iface));
  if(strlen(iface) == 0 && Default_mcast_iface != NULL)
    strlcpy(iface,Default_mcast_iface,sizeof(iface));

  if(output == 0)
    return listen_mcast(sock,iface);
  else
    return connect_mcast(sock,iface,ttl,tos);
}

// Create a socket for sending to a multicast group
int connect_mcast(void const *s,char const *iface,int const ttl,int const tos){
  if(s == NULL)
    return -1;

  struct sockaddr const *sock = s;

  int fd = socket(sock->sa_family,SOCK_DGRAM,0);

  if(fd == -1){
    perror("setup_mcast socket");
    return -1;
  }      
  // Strictly speaking, it is not necessary to join a multicast group to which we only send.
  // But this creates a problem with "smart" switches that do IGMP snooping.
  // They have a setting to handle what happens with unregistered
  // multicast groups (groups to which no IGMP messages are seen.)
  // Discarding unregistered multicast breaks IPv6 multicast, which breaks ALL of IPv6
  // because neighbor discovery uses multicast.
  // It can also break IPv4 mDNS, though hardwiring 224.0.0.251 to flood can fix this.
  // But if the switches are set to pass unregistered multicasts, then IPv4 multicasts
  // that aren't subscribed to by anybody are flooded everywhere!
  // We avoid that by subscribing to our own multicasts.
  switch(sock->sa_family){
  case AF_INET:
    set_ipv4_options(fd,ttl,tos);
    if(ipv4_join_group(fd,sock,iface) != 0)
      fprintf(stderr,"connect_mcast join_group failed\n");
    break;
  case AF_INET6:
    set_ipv6_options(fd,ttl,tos);
    if(ipv6_join_group(fd,sock,iface) != 0)
      fprintf(stderr,"connect_mcast join_group failed\n");
    break;
  default:
    return -1;
  }

  // Better to drop a packet than to block real-time processing
  fcntl(fd,F_SETFL,O_NONBLOCK);
  if(connect(fd,sock,sizeof(struct sockaddr)) != 0){
    perror("connect mcast");
    close(fd);
    return -1;
  }
  return fd;
}

// Create a listening socket on specified socket, using specified interface
// Interface may be null
int listen_mcast(void const *s,char const *iface){
  if(s == NULL)
    return -1;

  struct sockaddr const *sock = s;

  int const fd = socket(sock->sa_family,SOCK_DGRAM,0);
  if(fd == -1){
    perror("setup_mcast socket");
    return -1;
  }      
  switch(sock->sa_family){
  case AF_INET:
    set_ipv4_options(fd,-1,-1);
    if(ipv4_join_group(fd,sock,iface) != 0)
     fprintf(stderr,"join_group failed\n");
    break;
  case AF_INET6:
    set_ipv6_options(fd,-1,-1);
    if(ipv6_join_group(fd,sock,iface) != 0)
     fprintf(stderr,"join_group failed\n");
    break;
  default:
    return -1;
  }
  if((bind(fd,sock,sizeof(struct sockaddr)) != 0)){
    perror("listen mcast bind");
    close(fd);
    return -1;
  }
  return fd;
}

// Resolve a multicast target string in the form "name[:port][,iface]"
// If "name" is not qualified (no periods) then .local will be appended by default
// If :port is not specified, port field in result will be zero
int resolve_mcast(char const *target,void *sock,int default_port,char *iface,int iface_len){
  if(target == NULL || strlen(target) == 0 || sock == NULL)
    return -1;

  char host[PATH_MAX]; // Maximum legal DNS name length?
  strlcpy(host,target,sizeof(host));

  // Look for ,iface at end of target. If present, delimit and copy to user
  char *ifp = strrchr(host,',');
  if(ifp != NULL){
    // ,iface field found
    *ifp++ = '\0'; // Zap ',' with null to end preceding string
  }
  if(iface != NULL && iface_len > 0){
    if(ifp == NULL)
      *iface = '\0';
    else
      strlcpy(iface,ifp,iface_len);
  }
  // Look for :port
  char *port;
  if((port = strrchr(host,':')) != NULL){
    *port++ = '\0';
  }

  struct addrinfo *results;
  int try;
  // If no domain zone is specified, assume .local (i.e., for multicast DNS)
  char full_host[PATH_MAX+6];
  if(strchr(host,'.') == NULL)
    snprintf(full_host,sizeof(full_host),"%s.local",host);
  else
    strlcpy(full_host,host,sizeof(full_host));
    
  for(try=0;;try++){
    results = NULL;
    struct addrinfo hints;
    memset(&hints,0,sizeof(hints));
#if 0
    // Using hints.ai_family = AF_UNSPEC generates both A and AAAA queries
    // but even when the A query is answered the library times out and retransmits the AAAA
    // query several times. So do only an A (IPv4) query the first time
    hints.ai_family = (try == 0) ? AF_INET : AF_UNSPEC;
#else
    // Doesn't seem to be a problem anymore, and using AF_INET often fails
    // on loopback.
    // Did this get changed recently in getaddrinfo()?
    hints.ai_family = AF_UNSPEC;
#endif
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    hints.ai_flags = AI_ADDRCONFIG;
    
    int const ecode = getaddrinfo(full_host,port,&hints,&results);
    if(ecode == 0)
      break;
    if(try == 0) // Don't pollute the syslog
      fprintf(stderr,"resolve_mcast getaddrinfo(%s,%s): %s. Retrying.\n",full_host,port,gai_strerror(ecode));
    sleep(10);
  }
  if(try > 0) // Don't leave them hanging: report success after failure
    fprintf(stderr,"resolve_mcast getaddrinfo(%s,%s) succeeded\n",full_host,port);

  // Use first entry on list -- much simpler
  // I previously tried each entry in turn until one succeeded, but with UDP sockets and
  // flags set to only return supported addresses, how could any of them fail?
  memcpy(sock,results->ai_addr,results->ai_addrlen);
  if(port == NULL){
    // Insert default port
    setportnumber(sock,default_port);
  }
  freeaddrinfo(results); results = NULL;
  return 0;
}
// Convert RTP header from network (wire) big-endian format to internal host structure
// Written to be insensitive to host byte order and C structure layout and padding
// Use of unsigned formats is important to avoid unwanted sign extension
void const *ntoh_rtp(struct rtp_header * const rtp,void const * const data){
  uint32_t const *dp = data;

  uint32_t const w = ntohl(*dp++);
  rtp->version = w >> 30;
  rtp->pad = (w >> 29) & 1;
  rtp->extension = (w >> 28) & 1;
  rtp->cc = (w >> 24) & 0xf;
  rtp->marker = (w >> 23) & 1;
  rtp->type = (w >> 16) & 0x7f;
  rtp->seq = w & 0xffff;

  rtp->timestamp = ntohl(*dp++);
  rtp->ssrc = ntohl(*dp++);

  for(int i=0; i<rtp->cc; i++)
    rtp->csrc[i] = ntohl(*dp++);

  if(rtp->extension){
    int ext_len = ntohl(*dp++) & 0xffff;    // Ignore any extension, but skip over it
    dp += ext_len;
  }
  return dp;
}


// Convert RTP header from internal host structure to network (wire) big-endian format
// Written to be insensitive to host byte order and C structure layout and padding
void *hton_rtp(void * const data, struct rtp_header const * const rtp){
  uint32_t *dp = data;
  int cc = rtp->cc & 0xf; // Ensure in range, <= 15
  *dp++ = htonl(RTP_VERS << 30 | rtp->pad << 29 | rtp->extension << 28 | cc << 24 | rtp->marker << 23
		| (rtp->type & 0x7f) << 16 | rtp->seq);
  *dp++ = htonl(rtp->timestamp);
  *dp++ = htonl(rtp->ssrc);
  for(int i=0; i < cc ; i++)
    *dp++ = htonl(rtp->csrc[i]);

  return dp;
}


// Process sequence number and timestamp in incoming RTP header:
// Check that the sequence number is (close to) what we expect
// If not, drop it but 3 wild sequence numbers in a row will assume a stream restart
//
// Determine timestamp jump, if any
// Returns: <0            if packet should be dropped as a duplicate or a wild sequence number
//           0            if packet is in sequence with no missing timestamps
//         timestamp jump if packet is in sequence or <10 sequence numbers ahead, with missing timestamps
int rtp_process(struct rtp_state * const state,struct rtp_header const * const rtp,int const sampcnt){
  if(rtp->ssrc != state->ssrc){
    // Normally this will happen only on the first packet in a session since
    // the caller demuxes the SSRC to multiple instances.
    // But a single-instance, interactive application like 'radio' lets the SSRC
    // change so it doesn't have to restart when the stream sender does.
    state->init = false;
    state->ssrc = rtp->ssrc; // Must be filtered elsewhere if you want it
  }
  if(!state->init){
    state->packets = 0;
    state->seq = rtp->seq;
    state->timestamp = rtp->timestamp;
    state->dupes = 0;
    state->drops = 0;
    state->init = true;
  }
  state->packets++;
  // Sequence number check
  int const seq_step = (int16_t)(rtp->seq - state->seq);
  if(seq_step != 0){
    if(seq_step < 0){
      state->dupes++;
      return -1;
    }
    state->drops += seq_step;
  }
  state->seq = rtp->seq + 1;

  int const time_step = (int32_t)(rtp->timestamp - state->timestamp);
  if(time_step < 0)
    return time_step;    // Old samples; drop. Shouldn't happen if sequence number isn't old

  state->timestamp = rtp->timestamp + sampcnt;
  return time_step;
}

// Convert binary sockaddr structure (v4 or v6) to printable numeric string

char *formataddr(char *result,int size,void const *s){
  struct sockaddr const *sa = (struct sockaddr *)s;
  result[0] = '\0';
  switch(sa->sa_family){
  case AF_INET:
    {
      struct sockaddr_in const *sin = (struct sockaddr_in *)sa;
      inet_ntop(AF_INET,&sin->sin_addr,result,size);
    }
    break;
  case AF_INET6:
    {
      struct sockaddr_in6 const *sin = (struct sockaddr_in6 *)sa;
      inet_ntop(AF_INET6,&sin->sin6_addr,result,size);
    }
    break;
  }
  return result;
}


// Convert binary sockaddr structure to printable host:port string
// cache result, as getnameinfo can be very slow when it doesn't get a reverse DNS hit
// Needs locks to be made thread safe

struct inverse_cache {
  struct inverse_cache *next;
  struct inverse_cache *prev;
  struct sockaddr_storage sock;
  char hostport [NI_MAXHOST+NI_MAXSERV+5];
};

static struct inverse_cache *Inverse_cache_table; // Head of cache linked list

// We actually take a sockaddr *, but can also accept a sockaddr_in *, sockaddr_in6 * and sockaddr_storage *
// so to make it easier for callers we just take a void * and avoid pointer casts that impair readability
char const *formatsock(void const *s){
  // Determine actual length (and type) of binary socket structure (IPv4/IPv6)
  int slen = 0;
  struct sockaddr const * const sa = (struct sockaddr *)s;
  if(sa == NULL)
    return NULL;
  switch(sa->sa_family){
  case AF_INET:
    slen = sizeof(struct sockaddr_in);
    break;
  case AF_INET6:
    slen = sizeof(struct sockaddr_in6);
    break;
  default: // shouldn't happen unless uninitialized
    return NULL;
  }

  for(struct inverse_cache *ic = Inverse_cache_table; ic != NULL; ic = ic->next){
    if(address_match(&ic->sock,sa) && getportnumber(&ic->sock) == getportnumber(sa)){
      if(ic->prev == NULL)
	return ic->hostport; // Already at top of list

      // move to top of list so it'll be faster to find if we look for it again soon
      ic->prev->next = ic->next;
      if(ic->next)
	ic->next->prev = ic->prev;
      
      ic->next = Inverse_cache_table;
      ic->next->prev = ic;
      ic->prev = NULL;
      Inverse_cache_table = ic;
      return ic->hostport;
    }
  }
  // Not in list yet, add at top
  struct inverse_cache * const ic = (struct inverse_cache *)calloc(1,sizeof(*ic));
  assert(ic != NULL); // Malloc failures are rare
  char host[NI_MAXHOST],port[NI_MAXSERV];
  memset(host,0,sizeof(host));
  memset(port,0,sizeof(port));  
  getnameinfo(sa,slen,
	      host,NI_MAXHOST,
	      port,NI_MAXSERV,
	      //		NI_NOFQDN|NI_NUMERICHOST|NI_NUMERICSERV);
	      NI_NOFQDN|NI_NUMERICSERV);
  snprintf(ic->hostport,sizeof(ic->hostport),"%s:%s",host,port);
  assert(slen < sizeof(ic->sock));
  memcpy(&ic->sock,sa,slen);
  // Put at head of table
  ic->next = Inverse_cache_table;
  if(ic->next)
    ic->next->prev = ic;
  Inverse_cache_table = ic;
  return ic->hostport;
}

char const *id_from_type(int const type){
  switch(type){
  case OPUS_PT:
    return "Opus";
  case PCM_MONO_PT:
  case PCM_STEREO_PT:
    return "PCM";
  case PCM_STEREO_24_PT:
  case PCM_MONO_24_PT:
    return "PCM24";
  case PCM_STEREO_16_PT:
  case PCM_MONO_16_PT:
    return "PCM16";
  case PCM_STEREO_12_PT:
  case PCM_MONO_12_PT:
    return "PCM12";
  case PCM_STEREO_8_PT:
  case PCM_MONO_8_PT:
    return "PCM8";
  default:
    return "";
  }
}

// Determine sample rate from RTP payload type
int samprate_from_pt(int const type){
  switch(type){
  case PCM_MONO_PT:
  case PCM_STEREO_PT:
  case OPUS_PT: // Internally 48 kHz, though not really applicable
    return 48000;
  case PCM_MONO_24_PT:
  case PCM_STEREO_24_PT:
    return 24000;
  case PCM_MONO_16_PT:
  case PCM_STEREO_16_PT:
    return 16000;
  case PCM_MONO_12_PT:
  case PCM_STEREO_12_PT:
    return 12000;
  case PCM_MONO_8_PT:
  case PCM_STEREO_8_PT:
    return 8000;
  default:
    return 0;
  }
}
int channels_from_pt(int const type){
  switch(type){
  case REAL_PT12:
  case REAL_PT:
  case REAL_PT8:
  case PCM_MONO_PT:
  case PCM_MONO_24_PT:
  case PCM_MONO_16_PT:
  case PCM_MONO_12_PT:
  case PCM_MONO_8_PT:
    return 1;
  case IQ_PT12:
  case IQ_PT8:
  case PCM_STEREO_PT:
  case OPUS_PT:
  case PCM_STEREO_24_PT:
  case PCM_STEREO_16_PT:
  case PCM_STEREO_12_PT:
  case PCM_STEREO_8_PT:
    return 2;
  default:
    return 0;
  }
}



int pt_from_info(int const samprate,int const channels){
  int s;
  switch(samprate){
  case 8000:
    s = 0;
    break;
  case 12000:
    s = 1;
    break;
  case 16000:
    s = 2;
    break;
  case 24000:
    s = 3;
    break;
    // other sample rates also use the original RTP definitions, so it's really "unspecified"
  case 48000:
  default:
    s = 4;
    break;
  }
  if(channels < 1 || channels > 2)
    return -1;

  return pt_table[s][channels-1];
}
 
// Compare IP addresses in sockaddr structures for equality
int address_match(void const *arg1,void const *arg2){
  struct sockaddr const *s1 = (struct sockaddr *)arg1;
  struct sockaddr const *s2 = (struct sockaddr *)arg2;
  if(s1->sa_family != s2->sa_family)
    return 0;

  switch(s1->sa_family){
  case AF_INET:
    {
      struct sockaddr_in const *sinp1 = (struct sockaddr_in *)arg1;
      struct sockaddr_in const *sinp2 = (struct sockaddr_in *)arg2;
      if(memcmp(&sinp1->sin_addr,&sinp2->sin_addr,sizeof(sinp1->sin_addr)) == 0)
	return 1;
    }
    break;
  case AF_INET6:
    {
      struct sockaddr_in6 const *sinp1 = (struct sockaddr_in6 *)arg1;
      struct sockaddr_in6 const *sinp2 = (struct sockaddr_in6 *)arg2;
      if(memcmp(&sinp1->sin6_addr,&sinp2->sin6_addr,sizeof(sinp1->sin6_addr)) == 0)
	return 1;
    }
    break;    
  }
  return 0;
}



// Return port number (in HOST order) in a sockaddr structure
// Return -1 on error
int getportnumber(void const *arg){
  if(arg == NULL)
    return -1;
  struct sockaddr const *sock = (struct sockaddr *)arg;

  switch(sock->sa_family){
  case AF_INET:
    {
      struct sockaddr_in const *sin = (struct sockaddr_in *)sock;
      return ntohs(sin->sin_port);
    }
    break;
  case AF_INET6:
    {
      struct sockaddr_in6 const *sin6 = (struct sockaddr_in6 *)sock;
      return ntohs(sin6->sin6_port);
    }
    break;
  default:
    return -1;
  }
}

// Set the port number on a sockaddr structure
// Port number argument is in HOST order
int setportnumber(void *s,uint16_t port){
  if(s == NULL)
    return -1;
  struct sockaddr *sock = s;

  switch(sock->sa_family){
  case AF_INET:
    {
      struct sockaddr_in *sin = (struct sockaddr_in *)sock;
      sin->sin_port = htons(port);
    }
    break;
  case AF_INET6:
    {
      struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)sock;
      sin6->sin6_port = htons(port);
    }
    break;
  default:
    return -1;
  }
  return 0;
}

// Set options on IPv4 multicast socket
static void set_ipv4_options(int const fd,int const mcast_ttl,int const tos){
  // Failures here are not fatal
#ifdef IP_FREEBIND
  int const freebind = true;
  if(setsockopt(fd,IPPROTO_IP,IP_FREEBIND,&freebind,sizeof(freebind)) != 0)
    perror("freebind failed");
#endif

  int const reuse = true; // bool doesn't work for some reason
  if(setsockopt(fd,SOL_SOCKET,SO_REUSEPORT,&reuse,sizeof(reuse)) != 0)
    perror("so_reuseport failed");
  if(setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse)) != 0)
    perror("so_reuseaddr failed");

  struct linger linger;
  linger.l_onoff = 0;
  linger.l_linger = 0;
  if(setsockopt(fd,SOL_SOCKET,SO_LINGER,&linger,sizeof(linger)) != 0)
    perror("so_linger failed");

  if(mcast_ttl >= 0){
    if(setsockopt(fd,IPPROTO_IP,IP_MULTICAST_TTL,&mcast_ttl,sizeof(mcast_ttl)) != 0)
      perror("so_ttl failed");
  }
  uint8_t const loop = true;
  if(setsockopt(fd,IPPROTO_IP,IP_MULTICAST_LOOP,&loop,sizeof(loop)) != 0)
    perror("so_loop failed");

  if(tos >= 0){
    // Only needed on output
    if(setsockopt(fd,IPPROTO_IP,IP_TOS,&tos,sizeof(tos)) != 0)
      perror("so_tos failed");
  }
}
// Set options on IPv6 multicast socket
static void set_ipv6_options(int const fd,int const mcast_ttl,int const tos){
  // Failures here are not fatal

  int const reuse = true; // bool doesn't work for some reason
  if(setsockopt(fd,SOL_SOCKET,SO_REUSEPORT,&reuse,sizeof(reuse)) != 0)
    perror("so_reuseport failed");
  if(setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse)) != 0)
    perror("so_reuseaddr failed");

  struct linger linger;
  linger.l_onoff = 0;
  linger.l_linger = 0;
  if(setsockopt(fd,SOL_SOCKET,SO_LINGER,&linger,sizeof(linger)) != 0)
    perror("so_linger failed");

  if(mcast_ttl >= 0){
    // Only needed on output
    uint8_t const ttl = mcast_ttl;
    if(setsockopt(fd,IPPROTO_IPV6,IPV6_MULTICAST_HOPS,&ttl,sizeof(ttl)) != 0)
      perror("so_ttl failed");
  }
  uint8_t const loop = 1;
  if(setsockopt(fd,IPPROTO_IPV6,IPV6_MULTICAST_LOOP,&loop,sizeof(loop)) != 0)
    perror("so_loop failed");

  if(tos >= 0){
    // Only needed on output
    if(setsockopt(fd,IPPROTO_IPV6,IPV6_TCLASS,&tos,sizeof(tos)) != 0)
      perror("so_tos failed");
  }
}

// Join a socket to a multicast group
static int ipv4_join_group(int const fd,void const * const sock,char const * const iface){
  if(fd < 0 || sock == NULL)
    return -1;

  // Ensure it's a multicast address
  // Is this check really necessary?
  // Maybe the setsockopt would just fail cleanly if it's not
  struct sockaddr_in const * const sin = (struct sockaddr_in *)sock;
  if(!IN_MULTICAST(ntohl(sin->sin_addr.s_addr)))
    return -1;
  
  struct ip_mreqn mreqn;
  mreqn.imr_multiaddr = sin->sin_addr;
  mreqn.imr_address.s_addr = INADDR_ANY;
  if(iface == NULL || strlen(iface) == 0)
    mreqn.imr_ifindex = 0;
  else
    mreqn.imr_ifindex = if_nametoindex(iface);
  if(setsockopt(fd,IPPROTO_IP,IP_ADD_MEMBERSHIP,&mreqn,sizeof(mreqn)) != 0){
    perror("multicast v4 join");
    return -1;
  }
  return 0;
}
static int ipv6_join_group(int const fd,void const * const sock,char const * const iface){
  if(fd < 0 || sock == NULL)
    return -1;

  // Ensure it's a multicast address
  // Is this check really necessary?
  // Maybe the setsockopt would just fail cleanly if it's not
  struct sockaddr_in6 const * const sin6 = (struct sockaddr_in6 *)sock;
  if(!IN6_IS_ADDR_MULTICAST(&sin6->sin6_addr))
    return -1;
  struct ipv6_mreq ipv6_mreq;
  ipv6_mreq.ipv6mr_multiaddr = sin6->sin6_addr;
  if(iface == NULL || strlen(iface) == 0)
    ipv6_mreq.ipv6mr_interface = 0; // Default interface
  else
    ipv6_mreq.ipv6mr_interface = if_nametoindex(iface);
  
  // Doesn't seem to be defined on Mac OSX, but is supposed to be synonymous with IPV6_JOIN_GROUP
#ifndef IPV6_ADD_MEMBERSHIP
#define IPV6_ADD_MEMBERSHIP IPV6_JOIN_GROUP      
#endif
  if(setsockopt(fd,IPPROTO_IP,IPV6_ADD_MEMBERSHIP,&ipv6_mreq,sizeof(ipv6_mreq)) != 0){
    perror("multicast v6 join");
    return -1;
  }
  return 0;
}

static struct {
  int flag;
  char const *name;
} flags[] = {{IFF_UP,"UP"},
	     {IFF_BROADCAST,"BROADCAST"},
	     {IFF_DEBUG,"DEBUG"},
	     {IFF_LOOPBACK,"LOOPBACK"},
	     {IFF_POINTOPOINT,"PTP"},
	     {IFF_RUNNING,"RUNNING"},
	     {IFF_NOARP,"NOARP"},
	     {IFF_PROMISC,"PROMISC"},
	     {IFF_NOTRAILERS,"NOTRAILERS"},
	     {IFF_ALLMULTI,"ALLMULTI"},
#ifdef IFF_MASTER
	     {IFF_MASTER,"MASTER"},
#endif
#ifdef IFF_SLAVE
	     {IFF_SLAVE,"SLAVE"},
#endif
	     {IFF_MULTICAST,"MULTICAST"},
#ifdef IFF_PORTSEL
	     {IFF_PORTSEL,"PORTSEL"},
#endif
#ifdef IFF_AUOMEDIA
	     {IFF_AUTOMEDIA,"AUTOMEDIA"},
#endif
#ifdef IFF_DYNAMIC
	     {IFF_DYNAMIC,"DYNAMIC"},
#endif
#ifdef IFF_LOWER_UP
	     {IFF_LOWER_UP,"LOWER_UP"},
#endif
#ifdef IFF_DORMANT
	     {IFF_DORMANT,"DORMANT"},
#endif
#ifdef IFF_ECHO	     
	     {IFF_ECHO,"ECHO"},
#endif
	     {0, NULL},
};


// Dump list of interfaces
void dump_interfaces(void){
  struct ifaddrs *ifap = NULL;
  
  getifaddrs(&ifap);
  fprintf(stdout,"Interface list:\n");
  
  for(struct ifaddrs const *i = ifap; i != NULL; i = i->ifa_next){
    int const family = i->ifa_addr->sa_family;

    char const *familyname = NULL;
    int socksize = 0;
    switch(family){
    case AF_INET:
      familyname = "AF_INET";
      socksize = sizeof(struct sockaddr_in);
      break;
    case AF_INET6:
      familyname = "AF_INET6";
      socksize = sizeof(struct sockaddr_in6);
      break;
#ifdef AF_LINK
    case AF_LINK:
      familyname = "AF_LINK";
      socksize = sizeof(struct sockaddr_dl);
      break;
#endif
#ifdef AF_PACKET
    case AF_PACKET:
      familyname = "AF_PACKET";
      socksize = sizeof(struct sockaddr_ll);
      break;
#endif
    default:
      familyname = "?";
      break;
    }
    fprintf(stdout,"%s %s(%d)",i->ifa_name,familyname,family);

    char host[NI_MAXHOST];
    
    if(i->ifa_addr && getnameinfo(i->ifa_addr,socksize,host,NI_MAXHOST,NULL,0,NI_NUMERICHOST) == 0)
      fprintf(stdout," addr %s",host);
    if(i->ifa_dstaddr && getnameinfo(i->ifa_dstaddr,socksize,host,NI_MAXHOST,NULL,0,NI_NUMERICHOST) == 0)
      fprintf(stdout," dstaddr %s",host);
    if(i->ifa_netmask && getnameinfo(i->ifa_netmask,socksize,host,NI_MAXHOST,NULL,0,NI_NUMERICHOST) == 0)
      fprintf(stdout," mask %s",host);
    if(i->ifa_data)
      fprintf(stdout," data %p",i->ifa_data);
    const int f = i->ifa_flags;
    for(int j=0;flags[j].flag != 0; j++){
      if(f & flags[j].flag)
	fprintf(stdout," %s",flags[j].name);
    }
    fprintf(stdout,"\n");
  }
  fprintf(stdout,"end of list\n");
  freeifaddrs(ifap);
  ifap = NULL;
}
