// Multicast socket and network utility routines for ka9q-radio
// Copyright 2018-2023 Phil Karn, KA9Q

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
#include <sys/un.h>
#include <sys/ioctl.h>
#include <ifaddrs.h>
#include <fcntl.h>
#include <errno.h>

#if defined(linux)
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <bsd/string.h>
#include <sys/prctl.h>
#include <linux/capability.h>
#endif

#ifdef __APPLE__
#include <net/if_dl.h>
#endif

#include "multicast.h"
#include "rtp.h"
#include "misc.h"

static int setup_ipv4_loopback(int fd);
static int ipv4_join_group(int const fd,void const * const sock,char const * const iface);
static int ipv6_join_group(int const fd,void const * const sock,char const * const iface);
int get_multicast_route(struct sockaddr_in *);
#if 0
int ensure_multicast_on_loopback(void);
#endif

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
int setup_mcast(char const * const target,struct sockaddr *sock,int const output,int const ttl,int const tos,int const offset,int tries){
  if(target == NULL && sock == NULL)
    return -1; // At least one must be supplied

  if(sock == NULL){
    sock = alloca(sizeof(struct sockaddr_storage));
    memset(sock,0,sizeof(struct sockaddr_storage));
  }
  char iface[1024];
  iface[0] = '\0';
  if(target){
    int ret = resolve_mcast(target,sock,DEFAULT_RTP_PORT+offset,iface,sizeof(iface),tries);
    if(ret == -1)
      return -1;
  }
  if(strlen(iface) == 0 && Default_mcast_iface != NULL)
    strlcpy(iface,Default_mcast_iface,sizeof(iface));

  if(output == 0)
    return listen_mcast(sock,iface);
  else
    return connect_mcast(sock,iface,ttl,tos);
}

// Join an existing socket to a multicast group without connecting it
// Since many channels may send to the same multicast group, the joins can often fail with harmless "address already in use" messages
// Note: only the IP address is significant, the port number is ignored
int join_group(int fd,struct sockaddr const * const sock, char const * const iface){
  if(fd == -1 || sock == NULL)
    return -1;

  switch(sock->sa_family){
  case AF_INET:
    return ipv4_join_group(fd,sock,iface);
    break;
  case AF_INET6:
    return ipv6_join_group(fd,sock,iface);
    break;
  default:
    return -1;
  }
  return 0;
}

// Set up a disconnected socket for output
// Like connect_mcast() but without the connect()
int output_mcast(void const * const s,char const * const iface,int const ttl,int const tos){
  if(s == NULL)
    return -1;

  struct sockaddr const *sock = s;
  int fd = socket(sock->sa_family,SOCK_DGRAM,0);
  if(fd == -1)
    return -1;

  // Better to drop a packet than to block real-time processing
  fcntl(fd,F_SETFL,O_NONBLOCK);
  if(ttl >= 0){
    // Only needed on output
    int mcast_ttl = ttl;
    int r = 0;
    if(sock->sa_family == AF_INET)
      r = setsockopt(fd,IPPROTO_IP,IP_MULTICAST_TTL,&mcast_ttl,sizeof(mcast_ttl));
    else
      r = setsockopt(fd,IPPROTO_IPV6,IPV6_MULTICAST_HOPS,&mcast_ttl,sizeof(mcast_ttl));
    if(r)
      perror("so_ttl failed");
  }
  // Ensure our local listeners get it too
  uint8_t const loop = true;
  int r = 0;
  if(sock->sa_family == AF_INET)
    r = setsockopt(fd,IPPROTO_IP,IP_MULTICAST_LOOP,&loop,sizeof(loop));
  else
    r = setsockopt(fd,IPPROTO_IPV6,IPV6_MULTICAST_LOOP,&loop,sizeof(loop));
  if(r)
    perror("so_loop failed");

  if(tos >= 0){
    int r = 0;
    if(sock->sa_family == AF_INET)
      r = setsockopt(fd,IPPROTO_IP,IP_TOS,&tos,sizeof(tos));
    else
      r = setsockopt(fd,IPPROTO_IPV6,IPV6_TCLASS,&tos,sizeof(tos));
    if(r)
      perror("so_tos failed");
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
  // As a side benefit, the join will probably fail if the interface is down, so we can attach to the loopback interface

  if(ttl == 0 || join_group(fd,sock,iface) == -1) // join_group will fail if the output interface isn't up
    setup_ipv4_loopback(fd); // attach it to the loopback interface

  return fd;
}

// Like output_mcast, but also do a connect()
int connect_mcast(void const * const s,char const * const iface,int const ttl,int const tos){
  int fd = output_mcast(s,iface,ttl,tos);
  if(fd == -1)
    return -1;

  if(connect(fd,s,sizeof(struct sockaddr)) == -1){
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
  if(join_group(fd,sock,iface) == -1){
    close(fd);
    return -1;
  }
  int const reuse = true; // bool doesn't work for some reason
  if(setsockopt(fd,SOL_SOCKET,SO_REUSEPORT,&reuse,sizeof(reuse)) != 0)
    perror("so_reuseport failed");
  if(setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse)) != 0)
    perror("so_reuseaddr failed");

#ifdef IP_FREEBIND
  int const freebind = true;
  if(setsockopt(fd,IPPROTO_IP,IP_FREEBIND,&freebind,sizeof(freebind)) != 0)
    perror("freebind failed");
#endif

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
int resolve_mcast(char const *target,void *sock,int default_port,char *iface,int iface_len,int tries){
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

  int64_t start_time = gps_time_ns();
  bool message_logged = false;

  for(try=0;tries == 0 || try != tries;try++){
    results = NULL;
    struct addrinfo hints;
    memset(&hints,0,sizeof(hints));
#if 1
    // Using hints.ai_family = AF_UNSPEC generates both A and AAAA queries
    // but even when the A query is answered the library times out and retransmits the AAAA
    // query several times. So do only an A (IPv4) query the first time
    hints.ai_family = (try == 0) ? AF_INET : AF_UNSPEC;
#else
    // using AF_INET often fails on loopback.
    // Did this get changed recently in getaddrinfo()?
    hints.ai_family = AF_UNSPEC;
#endif
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    hints.ai_flags = AI_ADDRCONFIG;

    int const ecode = getaddrinfo(full_host,port,&hints,&results);
    if(ecode == 0)
      break;
    if(!message_logged){
      int64_t now = gps_time_ns();
      if(now > start_time + 2 * BILLION){
	fprintf(stderr,"resolve_mcast getaddrinfo(host=%s, port=%s): %s. Retrying.\n",full_host,port,gai_strerror(ecode));
	message_logged = true;
      }
    }
  }
  if(message_logged && try == tries){
    fprintf(stderr,"resolve_mcast getaddrinfo(host=%s, port=%s) failed\n",full_host,port);
    return -1;
  }
  if(message_logged) // Don't leave them hanging: report success after failure
    fprintf(stderr,"resolve_mcast getaddrinfo(host=%s, port=%s) succeeded\n",full_host,port);

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

// Convert binary sockaddr structure (v4 or v6 or unix) to printable numeric string
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

// Needs locks to be made thread safe.
// Unfortunately, getnameinfo() can be very slow (the whole reason we need a cache!)
// so we let go of the lock while it executes. That might cause duplicate entries
// if two callers look up the same unresolved name at the same time, but that doesn't
// seem likely to cause a problem?

struct inverse_cache {
  struct inverse_cache *next;
  struct inverse_cache *prev;
  struct sockaddr_storage sock;
  char hostport [2*NI_MAXHOST+NI_MAXSERV+5];
};

static struct inverse_cache *Inverse_cache_table; // Head of cache linked list

static pthread_mutex_t Formatsock_mutex = PTHREAD_MUTEX_INITIALIZER;

// We actually take a sockaddr *, but can also accept a sockaddr_in *, sockaddr_in6 * and sockaddr_storage *
// so to make it easier for callers we just take a void * and avoid pointer casts that impair readability
char const *formatsock(void const *s,bool full){
  // Determine actual length (and type) of binary socket structure (IPv4/IPv6)
  size_t slen = 0;
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
  pthread_mutex_lock(&Formatsock_mutex);
  for(struct inverse_cache *ic = Inverse_cache_table; ic != NULL; ic = ic->next){
    if(address_match(&ic->sock,sa) && getportnumber(&ic->sock) == getportnumber(sa)){
      if(ic->prev == NULL){
	pthread_mutex_unlock(&Formatsock_mutex);
	return ic->hostport; // Already at top of list
      }
      // move to top of list so it'll be faster to find if we look for it again soon
      ic->prev->next = ic->next;
      if(ic->next)
	ic->next->prev = ic->prev;

      ic->next = Inverse_cache_table;
      ic->next->prev = ic;
      ic->prev = NULL;
      Inverse_cache_table = ic;
      pthread_mutex_unlock(&Formatsock_mutex);
      return ic->hostport;
    }
  }
  pthread_mutex_unlock(&Formatsock_mutex); // Let go of the lock, this will take a while
  // Not in list yet
  struct inverse_cache * const ic = (struct inverse_cache *)calloc(1,sizeof(*ic));
  assert(ic != NULL); // Malloc failures are rare
  char host[NI_MAXHOST],port[NI_MAXSERV],hostname[NI_MAXHOST];
  memset(host,0,sizeof(host));
  memset(hostname,0,sizeof(host));
  memset(port,0,sizeof(port));
  getnameinfo(sa,slen,
	      host,NI_MAXHOST,
	      port,NI_MAXSERV,
	      NI_NOFQDN|NI_NUMERICHOST|NI_NUMERICSERV); // this should be fast

  // Inverse search for name of 0.0.0.0 will time out after a long time
  if(full && strcmp(host,"0.0.0.0") != 0){
    getnameinfo(sa,slen,
		hostname,NI_MAXHOST,
		NULL,0,
		NI_NOFQDN|NI_NUMERICSERV);
  }
  if(full && strlen(hostname) > 0 && strncmp(hostname,host,sizeof(host)) != 0)
    snprintf(ic->hostport,sizeof(ic->hostport),"%s(%s):%s",host,hostname,port);
  else
    snprintf(ic->hostport,sizeof(ic->hostport),"%s:%s",host,port);

  assert(slen < sizeof(ic->sock));
  memcpy(&ic->sock,sa,slen);

  // Put at head of table
  pthread_mutex_lock(&Formatsock_mutex);
  ic->next = Inverse_cache_table;
  if(ic->next)
    ic->next->prev = ic;
  Inverse_cache_table = ic;
  pthread_mutex_unlock(&Formatsock_mutex);
  return ic->hostport;
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

// We need the multicast flag on the loopback interface
// Force it on if we have network admin capability
static void set_loopback_multicast(struct ifaddrs const *lop,int fd){
  if(!(lop->ifa_flags & IFF_MULTICAST)) {
    struct ifreq ifr = {0};
    strncpy(ifr.ifr_name,lop->ifa_name,IFNAMSIZ-1);
    ifr.ifr_flags = lop->ifa_flags | IFF_MULTICAST;
    if (ioctl(fd, SIOCSIFFLAGS, &ifr) < 0) {
      printf("Can't enable multicast option on loopback interface %s\n",ifr.ifr_name);
      perror("ioctl (set flags)");
    } else {
      printf("Multicast enabled on loopback interface %s\n",ifr.ifr_name);
#if __linux__
      if (prctl(PR_CAP_AMBIENT_LOWER, CAP_NET_ADMIN, 0, 0, 0) == -1) {
	perror("Failed to drop CAP_NET_ADMIN");
      }
#endif
    }
  }
}
// Configure outbound multicast socket for loopback, e.g., when TTL = 0 or operating standalone
static int setup_ipv4_loopback(int fd){
  // Instead of hardwiring the loopback name (which can vary) find it in the system's list
  struct ifaddrs *ifap = NULL;
  if(getifaddrs(&ifap) == -1)
    return -1;
  // Look for the loopback interface
  struct ifaddrs const *ifp = NULL;
  for(ifp = ifap; ifp != NULL; ifp = ifp->ifa_next){
    if(ifp->ifa_addr != NULL
       && ifp->ifa_addr->sa_family == AF_INET
       && (ifp->ifa_flags & IFF_LOOPBACK))
      break;
  }
  if(ifp == NULL){
    fprintf(stderr,"Can't find loopback interface");
    return -1;
  }
  struct sockaddr_in const *sin = (struct sockaddr_in *)ifp->ifa_addr;
  if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &sin->sin_addr, sizeof sin->sin_addr) < 0)
    perror("setsockopt IP_MULTICAST_IF failed");
  return fd;
}
// Join a socket to a multicast group on specified iface, or default if NULL
// Also join on loopback interfacd
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
  if(setsockopt(fd,IPPROTO_IP,IP_ADD_MEMBERSHIP,&mreqn,sizeof(mreqn)) != 0 && errno != EADDRINUSE)
    fprintf(stderr,"join IPv4 group %s on %s failed: %s\n",formatsock(sock,false),iface,strerror(errno));

  // Also join on multicast inteface
  // Instead of hardwiring the loopback name (which can vary) find it in the system's list
  struct ifaddrs *ifap = NULL;
  if(getifaddrs(&ifap) != 0)
    return -1;
  struct ifaddrs *lop = NULL;
  for(lop = ifap; lop != NULL; lop = lop->ifa_next){
    if(lop->ifa_addr != NULL && lop->ifa_addr->sa_family == AF_INET && (lop->ifa_flags & IFF_LOOPBACK))
      break;
  }
  if(lop != NULL){
    set_loopback_multicast(lop,fd); // Ensure MULTICAST flag is set
    mreqn.imr_ifindex = if_nametoindex(lop->ifa_name);
    if(setsockopt(fd,IPPROTO_IP,IP_ADD_MEMBERSHIP,&mreqn,sizeof(mreqn)) != 0 && errno != EADDRINUSE){
      perror("multicast loopback v4 join");
    }
  }
  freeifaddrs(ifap);
  return 0;
}

// Join a IPv6 socket to a multicast group on specified iface, or default if NULL
// Also join on loopback interfacd
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
  if(setsockopt(fd,IPPROTO_IP,IPV6_ADD_MEMBERSHIP,&ipv6_mreq,sizeof(ipv6_mreq)) != 0 && errno != EADDRINUSE)
    fprintf(stderr,"join IPv4 group %s on %s failed: %s\n",formatsock(sock,false),iface ? iface : "default",strerror(errno));

  // Instead of hardwiring the loopback name (which can vary) find it in the system's list
  struct ifaddrs *ifap = NULL;
  if(getifaddrs(&ifap) != 0)
    return -1;
  struct ifaddrs *lop = NULL;
  for(lop = ifap; lop != NULL; lop = lop->ifa_next){
    if(lop->ifa_addr->sa_family == AF_INET6 && (lop->ifa_flags & IFF_LOOPBACK)){
      ipv6_mreq.ipv6mr_interface = if_nametoindex(lop->ifa_name);
      if(setsockopt(fd,IPPROTO_IP,IPV6_ADD_MEMBERSHIP,&ipv6_mreq,sizeof ipv6_mreq) != 0 && errno != EADDRINUSE){
	fprintf(stderr,"join IPv6 group %s on %s failed: %s\n",formatsock(sock,false),iface ? iface : "default",strerror(errno));
      }
      break;
    }
  }
  freeifaddrs(ifap);
  return 0;
}
// Generate a multicast address in the 239.0.0.0/8 administratively scoped block
// avoiding 239.0.0.0/24 and 239.128.0.0/24 since these map at the link layer
// into the same Ethernet multicast MAC addresses as the 224.0.0.0/8 multicast control block
// that is not snooped by switches
uint32_t make_maddr(char const *arg){
  //  uint32_t addr = (239U << 24) | (ElfHashString(arg) & 0xffffff); // poor performance when last byte is always the same (.)
  uint32_t addr = (239U << 24) | (fnv1hash((uint8_t *)arg,strlen(arg)) & 0xffffff);
  // avoid 239.0.0.0/24 and 239.128.0.0/24 since they map to the same
  // Ethernet multicast MAC addresses as 224.0.0.0/24, the internet control block
  // This increases the risk of collision slightly (512 out of 16 M)
  if((addr & 0x007fff00) == 0)
    addr |= (addr & 0xff) << 8;
  if((addr & 0x007fff00) == 0)
    addr |= 0x00100000; // Small chance of this for a random address
  return addr;
}
