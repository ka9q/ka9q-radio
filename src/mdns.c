// Portable mDNS/DNS-SD implementation of the ka9q service-discovery interface
// declared in avahi.h, built on the dns_sd.h (Bonjour/mDNSResponder) API.
//
// This is the counterpart to avahi.c/avahi_browse.c (which shell out to the
// Linux avahi-* CLI tools). It lets radiod/control publish and browse services
// through the dns_sd.h API instead, on any platform that provides it:
//   - macOS   -- mDNSResponder, native in libSystem
//   - FreeBSD -- mDNSResponder port
//   - Linux   -- Avahi's mDNSResponder-compatible API (libavahi-compat-libdns_sd)
// The build system selects exactly one backend. On Linux the native avahi CLI
// backend is the default; this dns_sd backend is available there as a build
// option (MDNS_BACKEND=dns_sd), though the Avahi compatibility shim is a less
// complete dns_sd.h implementation than a full mDNSResponder (notably around
// DNSServiceGetAddrInfo and shared connections).
//
// The three entry points intentionally keep the historical avahi_* names and
// the exact struct service_tab allocation contract (all strings for one row
// packed into a single ->buffer freed by avahi_free_service_table), so no
// consumer needs to change.

#include <dns_sd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <ifaddrs.h>
#include <sys/select.h>
#include "avahi.h"

// Historical flag; the static-file publish mode (/etc/avahi/{hosts,services})
// is Avahi-on-Linux specific and has no dns_sd equivalent, so this backend
// always registers live. Defined here to satisfy the extern in avahi.h.
bool Static_avahi;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static int64_t now_ms(void){
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC,&ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// ---------------------------------------------------------------------------
// Publish side: avahi_start()
//
// All registrations share one DNSServiceCreateConnection() ref, serviced for
// the life of the process by a single background thread pumping
// DNSServiceProcessResult(). This is the in-process analog of the persistent
// avahi-publish-service/-address child processes in avahi.c.
// ---------------------------------------------------------------------------

static pthread_mutex_t Reg_lock = PTHREAD_MUTEX_INITIALIZER;
static DNSServiceRef Reg_conn = NULL;      // shared connection for all registrations
static pthread_t Reg_thread;
static bool Reg_running = false;

static void reg_service_reply(DNSServiceRef sdRef,DNSServiceFlags flags,DNSServiceErrorType err,
			      char const *name,char const *regtype,char const *domain,void *ctx){
  (void)sdRef;(void)flags;(void)domain;(void)ctx;
  if(err != kDNSServiceErr_NoError)
    fprintf(stderr,"mdns: register '%s' %s failed: %d\n",name?name:"?",regtype?regtype:"?",err);
}

static void reg_record_reply(DNSServiceRef sdRef,DNSRecordRef rec,DNSServiceFlags flags,
			     DNSServiceErrorType err,void *ctx){
  (void)sdRef;(void)rec;(void)flags;(void)ctx;
  if(err != kDNSServiceErr_NoError)
    fprintf(stderr,"mdns: register address record failed: %d\n",err);
}

// Service the shared connection forever.
static void *reg_pump(void *arg){
  (void)arg;
  for(;;){
    pthread_mutex_lock(&Reg_lock);
    int fd = Reg_conn ? DNSServiceRefSockFD(Reg_conn) : -1;
    pthread_mutex_unlock(&Reg_lock);
    if(fd < 0){
      usleep(100000);
      continue;
    }
    fd_set r;
    FD_ZERO(&r);
    FD_SET(fd,&r);
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    int s = select(fd+1,&r,NULL,NULL,&tv);
    if(s > 0 && FD_ISSET(fd,&r)){
      pthread_mutex_lock(&Reg_lock);
      if(Reg_conn)
	DNSServiceProcessResult(Reg_conn);
      pthread_mutex_unlock(&Reg_lock);
    }
  }
  return NULL;
}

// Ensure the shared connection + pump thread exist. Caller holds Reg_lock.
static int reg_ensure_conn(void){
  if(Reg_conn != NULL)
    return 0;
  if(DNSServiceCreateConnection(&Reg_conn) != kDNSServiceErr_NoError){
    Reg_conn = NULL;
    return -1;
  }
  if(!Reg_running){
    if(pthread_create(&Reg_thread,NULL,reg_pump,NULL) == 0){
      pthread_detach(Reg_thread);
      Reg_running = true;
    }
  }
  return 0;
}

int avahi_start(char const *service_name,char const *service_type,int const service_port,
		char const *dns_name,int address,char const *description){
  if(service_name == NULL || service_type == NULL)
    return -1;

  pthread_mutex_lock(&Reg_lock);
  if(reg_ensure_conn() != 0){
    pthread_mutex_unlock(&Reg_lock);
    fprintf(stderr,"mdns: DNSServiceCreateConnection failed\n");
    return -1;
  }

  // Build TXT records: pid, description, source hostname -- mirrors avahi.c.
  TXTRecordRef txt;
  TXTRecordCreate(&txt,0,NULL);
  {
    char pidstr[16];
    snprintf(pidstr,sizeof pidstr,"%d",(int)getpid());
    TXTRecordSetValue(&txt,"pid",(uint8_t)strlen(pidstr),pidstr);

    if(description != NULL && *description != '\0')
      TXTRecordSetValue(&txt,"description",(uint8_t)strlen(description),description);

    char hostname[256] = {0};
    gethostname(hostname,sizeof hostname - 1);
    TXTRecordSetValue(&txt,"source",(uint8_t)strlen(hostname),hostname);
  }

  DNSServiceRef svc = Reg_conn;   // sub-op on the shared connection
  DNSServiceErrorType e = DNSServiceRegister(&svc,
					     kDNSServiceFlagsShareConnection,
					     0,                          // any interface
					     service_name,
					     service_type,
					     NULL,                       // default domain (local)
					     dns_name,                   // SRV target host
					     htons((uint16_t)service_port),
					     TXTRecordGetLength(&txt),
					     TXTRecordGetBytesPtr(&txt),
					     reg_service_reply,NULL);
  TXTRecordDeallocate(&txt);
  if(e != kDNSServiceErr_NoError){
    pthread_mutex_unlock(&Reg_lock);
    fprintf(stderr,"mdns: DNSServiceRegister('%s','%s') failed: %d\n",service_name,service_type,e);
    return -1;
  }

  // Publish the name -> multicast-address A record (avahi.c's publish-address).
  // 'address' is a host-order packed IPv4 quad ((a<<24)|(b<<16)|(c<<8)|d).
  if(address != 0 && dns_name != NULL){
    char fullname[256];
    size_t n = strlen(dns_name);
    if(n > 0 && dns_name[n-1] == '.')
      snprintf(fullname,sizeof fullname,"%s",dns_name);           // already qualified
    else if(strchr(dns_name,'.') != NULL)
      snprintf(fullname,sizeof fullname,"%s.",dns_name);          // has a domain, add root dot
    else
      snprintf(fullname,sizeof fullname,"%s.local.",dns_name);    // bare host -> .local.

    uint32_t rdata = htonl((uint32_t)address);
    DNSRecordRef rec;
    e = DNSServiceRegisterRecord(Reg_conn,&rec,
				 kDNSServiceFlagsShared,
				 0,
				 fullname,
				 kDNSServiceType_A,
				 kDNSServiceClass_IN,
				 sizeof rdata,&rdata,
				 120,                    // ttl
				 reg_record_reply,NULL);
    if(e != kDNSServiceErr_NoError)
      fprintf(stderr,"mdns: DNSServiceRegisterRecord('%s') failed: %d\n",fullname,e);
  }
  pthread_mutex_unlock(&Reg_lock);
  return 0;
}

// ---------------------------------------------------------------------------
// Browse side: avahi_browse()
//
// dns_sd browse/resolve/getaddrinfo are async; avahi_browse() is synchronous
// with a bounded timeout. We drive one shared connection through the pipeline
//   DNSServiceBrowse -> DNSServiceResolve -> DNSServiceGetAddrInfo
// collecting completed rows into the caller's table, then sort + de-dupe by
// instance name exactly like avahi_browse.c.
// ---------------------------------------------------------------------------

struct browse_state {
  struct service_tab *table;
  int tabsize;
  int count;
  char const *service_name;
  DNSServiceRef conn;
  struct browse_ctx **ctxs;   // per-instance contexts to free at teardown
  int nctx, capctx;
};

struct browse_ctx {
  struct browse_state *st;
  uint32_t ifindex;
  char name[128];
  char host[256];
  char port[16];
  char txt[512];
};

static char *append_str(char **w,char const *s){
  char *start = *w;
  size_t n = strlen(s) + 1;
  memcpy(*w,s,n);
  *w += n;
  return start;
}

// Pack one resolved instance into the table using the single-buffer contract.
static void add_entry(struct browse_state *st,char const *name,char const *host,
		      char const *addr,char const *port,char const *iface,char const *txt){
  if(st->count >= st->tabsize)
    return;
  char const *type = st->service_name;
  size_t total = strlen("=")+1 + strlen(iface)+1 + strlen("IPv4")+1 + strlen(name)+1
    + strlen(type)+1 + strlen("local")+1 + strlen(host)+1 + strlen(addr)+1
    + strlen(port)+1 + strlen(txt)+1;
  char *buf = malloc(total);
  if(buf == NULL)
    return;
  char *w = buf;
  struct service_tab *tp = &st->table[st->count];
  memset(tp,0,sizeof *tp);
  tp->buffer    = buf;
  tp->line_type = append_str(&w,"=");
  tp->interface = append_str(&w,iface);
  tp->protocol  = append_str(&w,"IPv4");
  tp->name      = append_str(&w,name);
  tp->type      = append_str(&w,type);
  tp->domain    = append_str(&w,"local");
  tp->dns_name  = append_str(&w,host);
  tp->address   = append_str(&w,addr);
  tp->port      = append_str(&w,port);
  tp->txt       = append_str(&w,txt);
  st->count++;
}

static void flatten_txt(uint16_t len,void const *rec,char *out,size_t outsize){
  out[0] = '\0';
  if(rec == NULL || len == 0)
    return;
  uint16_t count = TXTRecordGetCount(len,rec);
  size_t used = 0;
  for(uint16_t i = 0; i < count; i++){
    char key[256];
    uint8_t vlen = 0;
    void const *val = NULL;
    if(TXTRecordGetItemAtIndex(len,rec,i,sizeof key,key,&vlen,&val) != kDNSServiceErr_NoError)
      break;
    int w = snprintf(out+used,outsize-used,"%s%s=%.*s",used?" ":"",key,(int)vlen,val?(char const *)val:"");
    if(w < 0 || (size_t)w >= outsize-used)
      break;
    used += w;
  }
}

static int track_ctx(struct browse_state *st,struct browse_ctx *c){
  if(st->nctx == st->capctx){
    int cap = st->capctx ? st->capctx * 2 : 16;
    struct browse_ctx **p = realloc(st->ctxs,cap * sizeof *p);
    if(p == NULL)
      return -1;
    st->ctxs = p;
    st->capctx = cap;
  }
  st->ctxs[st->nctx++] = c;
  return 0;
}

static void addr_cb(DNSServiceRef sdRef,DNSServiceFlags flags,uint32_t ifindex,
		    DNSServiceErrorType err,char const *hostname,
		    struct sockaddr const *address,uint32_t ttl,void *ctx){
  (void)sdRef;(void)flags;(void)ifindex;(void)hostname;(void)ttl;
  struct browse_ctx *c = ctx;
  if(err != kDNSServiceErr_NoError || address == NULL || address->sa_family != AF_INET)
    return;
  char ip[INET_ADDRSTRLEN] = "";
  struct sockaddr_in const *sin = (struct sockaddr_in const *)address;
  inet_ntop(AF_INET,&sin->sin_addr,ip,sizeof ip);

  char ifname[IF_NAMESIZE] = "";
  if(c->ifindex != 0)
    if_indextoname(c->ifindex,ifname);

  char host[256];
  snprintf(host,sizeof host,"%s",c->host);
  size_t hl = strlen(host);
  if(hl > 0 && host[hl-1] == '.')         // strip trailing root dot for display
    host[hl-1] = '\0';

  add_entry(c->st,c->name,host,ip,c->port,ifname,c->txt);
}

static void resolve_cb(DNSServiceRef sdRef,DNSServiceFlags flags,uint32_t ifindex,
		       DNSServiceErrorType err,char const *fullname,char const *hosttarget,
		       uint16_t port,uint16_t txtLen,unsigned char const *txtRecord,void *ctx){
  (void)sdRef;(void)flags;(void)fullname;
  struct browse_ctx *c = ctx;
  if(err != kDNSServiceErr_NoError)
    return;
  snprintf(c->host,sizeof c->host,"%s",hosttarget);
  snprintf(c->port,sizeof c->port,"%u",(unsigned)ntohs(port));
  flatten_txt(txtLen,txtRecord,c->txt,sizeof c->txt);
  c->ifindex = ifindex;

  DNSServiceRef r = c->st->conn;
  DNSServiceGetAddrInfo(&r,kDNSServiceFlagsShareConnection,ifindex,
			kDNSServiceProtocol_IPv4,hosttarget,addr_cb,c);
}

static void browse_cb(DNSServiceRef sdRef,DNSServiceFlags flags,uint32_t ifindex,
		      DNSServiceErrorType err,char const *serviceName,
		      char const *regtype,char const *replyDomain,void *ctx){
  (void)sdRef;
  struct browse_state *st = ctx;
  if(err != kDNSServiceErr_NoError || !(flags & kDNSServiceFlagsAdd))
    return;
  struct browse_ctx *c = calloc(1,sizeof *c);
  if(c == NULL)
    return;
  c->st = st;
  snprintf(c->name,sizeof c->name,"%s",serviceName);
  if(track_ctx(st,c) != 0){
    free(c);
    return;
  }
  DNSServiceRef r = st->conn;
  DNSServiceResolve(&r,kDNSServiceFlagsShareConnection,ifindex,
		    serviceName,regtype,replyDomain,resolve_cb,c);
}

// True if 'name' is a loopback interface. Used to break ties when the same
// instance is reported on more than one interface; see avahi_browse().
static bool iface_is_loopback(char const *name){
  if(name == NULL || *name == '\0')
    return false;
  struct ifaddrs *ifa = NULL;
  if(getifaddrs(&ifa) != 0)
    return false;
  bool loopback = false;
  for(struct ifaddrs *p = ifa; p != NULL; p = p->ifa_next){
    if(p->ifa_name != NULL && strcmp(p->ifa_name,name) == 0){
      loopback = (p->ifa_flags & IFF_LOOPBACK) != 0;
      break;
    }
  }
  freeifaddrs(ifa);
  return loopback;
}

// Sort by instance name; NULLs to the end (matches avahi_browse.c).
static int table_compare(void const *a,void const *b){
  struct service_tab const *t1 = a;
  struct service_tab const *t2 = b;
  if(t2 == NULL || t2->name == NULL)
    return -1;
  if(t1 == NULL || t1->name == NULL)
    return +1;
  return strcmp(t1->name,t2->name);
}

int avahi_browse(struct service_tab *table,int tabsize,char const *service_name){
  if(service_name == NULL || *service_name == '\0' || table == NULL || tabsize <= 0)
    return 0;
  memset(table,0,sizeof(*table) * tabsize);

  struct browse_state st = {0};
  st.table = table;
  st.tabsize = tabsize;
  st.service_name = service_name;

  if(DNSServiceCreateConnection(&st.conn) != kDNSServiceErr_NoError)
    return 0;

  DNSServiceRef browse = st.conn;
  if(DNSServiceBrowse(&browse,kDNSServiceFlagsShareConnection,0,
		      service_name,NULL,browse_cb,&st) != kDNSServiceErr_NoError){
    DNSServiceRefDeallocate(st.conn);
    return 0;
  }

  int fd = DNSServiceRefSockFD(st.conn);
  int64_t const start = now_ms();
  int64_t last_event = start;
  int64_t const budget = 2500;   // total ms to wait for discovery
  int64_t const idle   = 600;    // stop early after this much quiet once we have results

  for(;;){
    int64_t const t = now_ms();
    int64_t const elapsed = t - start;
    if(elapsed >= budget)
      break;
    if(st.count > 0 && (t - last_event) >= idle)
      break;

    int64_t remaining = budget - elapsed;
    int64_t slice = remaining < 200 ? remaining : 200;
    struct timeval tv = { .tv_sec = slice/1000, .tv_usec = (slice%1000)*1000 };
    fd_set r;
    FD_ZERO(&r);
    FD_SET(fd,&r);
    int s = select(fd+1,&r,NULL,NULL,&tv);
    if(s > 0 && FD_ISSET(fd,&r)){
      if(DNSServiceProcessResult(st.conn) != kDNSServiceErr_NoError)
	break;
      last_event = now_ms();
    } else if(s < 0 && errno != EINTR){
      break;
    }
  }

  // Deallocating the shared connection cleans up all sub-operations.
  DNSServiceRefDeallocate(st.conn);
  for(int i = 0; i < st.nctx; i++)
    free(st.ctxs[i]);
  free(st.ctxs);

  // Sort and remove duplicate instance names (same policy as avahi_browse.c).
  // mDNSResponder reports a locally published service on every interface it is
  // visible on, including loopback, so the same instance routinely arrives more
  // than once. control(1) passes the surviving row's ->interface straight to
  // listen_mcast()/join_group(), where a loopback interface would silently cut
  // it off from the network -- so when duplicates differ, keep a non-loopback
  // one rather than whichever qsort happened to leave first.
  qsort(table,st.count,sizeof table[0],table_compare);
  int dupes = 0;
  for(int i = 0; i < st.count; i++){
    if(table[i].buffer == NULL || table[i].name == NULL)
      continue;
    bool keep_is_loopback = iface_is_loopback(table[i].interface);
    for(int j = i+1; j < st.count; j++){
      if(table[j].buffer == NULL || table[j].name == NULL)
	continue;
      if(strcmp(table[i].name,table[j].name) != 0)
	break;
      if(keep_is_loopback && !iface_is_loopback(table[j].interface)){
	struct service_tab const tmp = table[i];   // promote the better row, discard the loopback one
	table[i] = table[j];
	table[j] = tmp;
	keep_is_loopback = false;
      }
      free(table[j].buffer);
      memset(&table[j],0,sizeof table[j]);
      dupes++;
    }
  }
  qsort(table,st.count,sizeof table[0],table_compare);
  return st.count - dupes;
}

// Free the per-row buffers; caller owns the table array itself.
void avahi_free_service_table(struct service_tab *table,int tabsize){
  for(int i = 0; i < tabsize; i++)
    free(table[i].buffer);
  memset(table,0,sizeof(*table) * tabsize);
}
