// $Id: avahi.c,v 1.21 2022/12/31 04:58:08 karn Exp $
// Adapted from avahi's example file client-publish-service.c
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <pthread.h>
#include <limits.h>
#include <avahi-client/client.h>
#include <avahi-client/publish.h>
#include <avahi-common/alternative.h>
#include <avahi-common/simple-watch.h>
#include <avahi-common/malloc.h>
#include <avahi-common/error.h>
#include <avahi-common/timeval.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include "misc.h"
#include "multicast.h"

struct userdata {
  char *service_name;
  char *service_type;
  int service_port;
  char *dns_name;
  char *description;
  uint32_t address;
  struct AvahiEntryGroup *group;
  AvahiSimplePoll *simple_poll;
  pthread_t avahi_thread;
  // Not really used yet; the caller just retries the lookups
  int ready;
  pthread_mutex_t avahi_mutex;
  pthread_cond_t avahi_ready;
  int service_name_collision;
};

#ifdef TRACE
static void dump_userdata(struct userdata const *u){
  fprintf(stderr,"service name %s\n",u->service_name);
  fprintf(stderr,"service type %s\n",u->service_type);  
  fprintf(stderr,"service port %d\n",u->service_port);
  fprintf(stderr,"dns_name %s\n",u->dns_name);
  fprintf(stderr,"description %s\n",u->description);
  char buffer[128];
  inet_ntop(AF_INET,&u->address,buffer,sizeof(buffer));
  fprintf(stderr,"address %s\n",buffer);
  fprintf(stderr,"group %p\n",u->group);
  fprintf(stderr,"simple_poll %p\n",u->simple_poll);
  fprintf(stderr,"avahi_thread %lu\n",(long unsigned)u->avahi_thread);
  fprintf(stderr,"ready %d\n",u->ready);
  fprintf(stderr,"service_name_collision %d\n",u->service_name_collision);
}
#endif


static int create_services(AvahiClient *c,struct userdata *userdata);
static void client_callback(AvahiClient *c, AvahiClientState state,void * userdata);
static void entry_group_callback(AvahiEntryGroup *g, AvahiEntryGroupState state,void *userdata);
void avahi_ready(struct userdata *);
static void *avahi_register(void *p);

extern int Verbose;

// description is optional; if present, forms a TXT record
// Optionally return a sockaddr_in (through *sock) with the resolved address
int avahi_start(char const *service_name,char const *service_type,int const service_port,char const *dns_name,int const base_address,char const *description,void *sock,int *socksize){
#if TRACE
  fprintf(stderr,"avahi_start(service_name = %s, service_type = %s, service_port = %d, dns_name = %s, base address = 0x%x, description = %s, sock = %p, socksize = %d\n",
	  service_name,service_type,service_port,dns_name,base_address,description,sock,*socksize);
#endif
  struct userdata *userdata = (struct userdata *)calloc(1,sizeof(struct userdata));
  if(service_name){
    // Look for ,iface at end of service_name and remove it, if present.
    char *ifp = strrchr(service_name,',');
    if(ifp != NULL){
      userdata->service_name = strndup(service_name,ifp-service_name+1);
      userdata->service_name[strlen(userdata->service_name)-1] = service_name[strlen(service_name)-1];
    } else
      userdata->service_name = strdup(service_name);
  }
  if(service_type)
    userdata->service_type = strdup(service_type);
  userdata->service_port = service_port;
  if(description)
    userdata->description = strdup(description); // Becomes TXT record
  if(dns_name){
    // Look for ,iface at end of dns_name and remove it, if present.
    char *ifp = strrchr(dns_name,',');
    if(ifp != NULL)
      userdata->dns_name = strndup(dns_name,ifp-dns_name);
    else
      userdata->dns_name = strdup(dns_name);
  }
  // force address into 239.x.x.x range
  userdata->address = htonl((0xefU << 24) + (base_address & 0xffffff));
  pthread_mutex_init(&userdata->avahi_mutex,NULL);
  pthread_cond_init(&userdata->avahi_ready,NULL);
  pthread_create(&userdata->avahi_thread,NULL,avahi_register,userdata);
  avahi_ready(userdata);
#ifdef TRACE
  fprintf(stderr,"avahi_start: \n");
  dump_userdata(userdata);
#endif
  if(sock != NULL && socksize != NULL){
    // Return sockaddr structure
    if(*socksize >= sizeof(struct sockaddr_in)){
      struct sockaddr_in *sin = sock;
      sin->sin_family = AF_INET;
      sin->sin_addr.s_addr = userdata->address;
      sin->sin_port = htons(service_port);
      *socksize = sizeof(struct sockaddr_in);
    } else
      *socksize = 0;
  }
  return 0;
}


static void *avahi_register(void *p){
  assert(p);
  pthread_setname("avahi-cli");
  pthread_detach(pthread_self());

  struct userdata *userdata = (struct userdata *)p;

  while(true){
    AvahiClient *client;
    int error;
    
    // Allocate main loop object
    if ((userdata->simple_poll = avahi_simple_poll_new()) == NULL){
      // This should be a very unlikely error; give up
      fprintf(stderr,"Failed to create simple poll object.\n");
      break;
    }
    // Allocate a new client
    client = avahi_client_new(avahi_simple_poll_get(userdata->simple_poll), AVAHI_CLIENT_NO_FAIL, client_callback, userdata, &error);
    if (client == NULL) {
      // This will happen if avahi-daemon isn't running; sleep and retry indefinitely
      fprintf(stderr,"Failed to create client: %s\n", avahi_strerror(error));
      assert(userdata->simple_poll);
      avahi_simple_poll_free(userdata->simple_poll);  userdata->simple_poll = NULL;
      sleep(5);
      continue;
    }
    for(;;){
      int r = avahi_simple_poll_iterate(userdata->simple_poll,-1);
      if(r != 0){
	if(Verbose)
	  fprintf(stderr,"avail_simple_poll_iterate(%p) returns %d\n",userdata->simple_poll,r);
	break;
      }
      int s = avahi_client_get_state(client);
      if(Verbose > 1){
	switch(s){
	case AVAHI_CLIENT_S_RUNNING:
	  fprintf(stderr,"Client state Running (%d)",s);
	  break;
	case AVAHI_CLIENT_FAILURE:
	  fprintf(stderr,"Client state Failure (%d)",s);
	  break;
	case AVAHI_CLIENT_S_COLLISION:
	  fprintf(stderr,"Client state Collision (%d)",s);
	  break;
	case AVAHI_CLIENT_S_REGISTERING:
	  fprintf(stderr,"Client state Registering (%d)",s);
	  break;
	case AVAHI_CLIENT_CONNECTING:
	  fprintf(stderr,"Client state Connecting (%d)",s);
	default:
	  fprintf(stderr,"Client state unknown (%d)",s);
	}
      }
      if(s == AVAHI_CLIENT_FAILURE)
	break; // avahi-daemon restarted
    }
    // Get here only on failure
    if(userdata->group){
      //      avahi_entry_group_reset(userdata->group);
      avahi_entry_group_free(userdata->group);
      userdata->group = NULL;
    }
    if (client){
      avahi_client_free(client);
      client = NULL;
    }
    if (userdata->simple_poll){
      avahi_simple_poll_free(userdata->simple_poll);
      userdata->simple_poll = NULL;
    }
    if(userdata->service_name_collision)
      break;
  }
  // Get here only on early failure; give up completely
  FREE(userdata->service_name);
  FREE(userdata->service_type);
  FREE(userdata->description);
  FREE(userdata->dns_name);
  FREE(userdata);
  pthread_exit(NULL);
}
static void client_callback(AvahiClient *c, AvahiClientState state, void *p) {
  struct userdata *userdata = (struct userdata *)p;

  assert(c);
  assert(p);
  switch (state) {
  case AVAHI_CLIENT_S_RUNNING:
    // The server has startup successfully and registered its host
    // name on the network, so it's time to create our services
    if(Verbose > 1)
      fprintf(stderr,"client_callback(client running)\n");

    if(create_services(c,userdata) != 0){
      avahi_simple_poll_quit(userdata->simple_poll);
      userdata->service_name_collision = 1;
    }
    break;
  case AVAHI_CLIENT_FAILURE:
    fprintf(stderr,"client callback: failure: %s\n", avahi_strerror(avahi_client_errno(c)));
    break;
  case AVAHI_CLIENT_S_COLLISION:
    if(Verbose)
      fprintf(stderr,"client_callback(client collision)\n");

    // Drop our registered services. When the server is back
    // in AVAHI_SERVER_RUNNING state we will register them
    // again with the new host name.
    if (userdata->group)
      avahi_entry_group_reset(userdata->group);
    break;
  case AVAHI_CLIENT_S_REGISTERING:
    if(Verbose > 1)
      fprintf(stderr,"client_callback(client registering)\n");

    // The server records are now being established. This
    // might be caused by a host name change. We need to wait
    // for our own records to register until the host name is
    // properly esatblished.
    if (userdata->group)
      avahi_entry_group_reset(userdata->group);
    break;
  case AVAHI_CLIENT_CONNECTING:
    if(Verbose > 1)
      fprintf(stderr,"client_callback(client connecting)\n");
    break;
  }
}

static int create_services(AvahiClient *c,struct userdata *userdata) {
  assert(c);
  assert(userdata);
  int records = 0;

#ifdef TRACE
  fprintf(stderr,"create_services\n");
  dump_userdata(userdata);
#endif

  if (!userdata->group){
    // First time we're called. Create a new entry
    userdata->group = avahi_entry_group_new(c, entry_group_callback, userdata);
    if (userdata->group == NULL) {
      fprintf(stderr,"avahi_entry_group_new() failed: %s\n", avahi_strerror(avahi_client_errno(c)));
      return -1;
    }
  }
#ifdef TRACE
  fprintf(stderr,"create_services 2\n");
  dump_userdata(userdata);
#endif


  // If the group is empty (either because it was just created, or
  // because it was reset previously, add our entries.
  if (avahi_entry_group_is_empty(userdata->group)) {
    if(Verbose > 1 && userdata->service_name)
      fprintf(stderr,"Adding services to '%s'\n", userdata->service_name);

    if(userdata->service_name && userdata->dns_name){
      // HOST_NAME_MAX not defined in consistent way across systems
      char ourname[1024];
      gethostname(ourname,sizeof(ourname));
      ourname[sizeof(ourname)-1] = '\0';
      char hosteq[sizeof(ourname)+100];
      snprintf(hosteq,sizeof(hosteq),"source=%s",ourname);
      hosteq[sizeof(hosteq)-1] = '\0';
      
      char pideq[1024];
      snprintf(pideq,sizeof(pideq),"pid=%d",getpid());
      pideq[sizeof(pideq)-1] = '\0';

      // Don't advertise on IPv6 for now since multicast addresses are IPv4-specific
      int ret = avahi_entry_group_add_service(userdata->group, AVAHI_IF_UNSPEC, AVAHI_PROTO_INET, 0, userdata->service_name, userdata->service_type, NULL, userdata->dns_name,
					      userdata->service_port,hosteq,pideq,userdata->description,NULL);
      if (ret < 0) {
	fprintf(stderr,"Failed to add service %s.%s: %s(%d)\n", userdata->service_name,userdata->service_type,avahi_strerror(ret),ret);
	return -1; // Fail even if collision; only one can exist per serial number
      }
      records++;
    }
    if(userdata->dns_name){
      // Add some multicast addresses in the 239.xx.xx.xx range
      AvahiAddress address;
      address.proto = AVAHI_PROTO_INET;
      int iter;

      for(iter=0; iter <= 100; iter++){
	// Be really paranoid and limit the number of iterations
	if(iter == 100)
	  return -1;

	address.data.ipv4.address = userdata->address;
	// Don't advertise on IPv6 for now since multicast addresses are IPv4-specific
	int ret = avahi_entry_group_add_address(userdata->group, AVAHI_IF_UNSPEC, address.proto, 0, userdata->dns_name,&address);
	if (ret == 0){
	  records++;
	  break;
	}
	if(iter == 0){
	  // Don't pollute the log
	  char temp[1024];
	  fprintf(stderr,"Failed to add address record %s (%s): %s %d\n", userdata->dns_name,inet_ntop(AF_INET,&address.data.ipv4.address,temp,sizeof(temp)),avahi_strerror(ret),ret);
	}
	if(ret != AVAHI_ERR_COLLISION)
	  return -1;
	userdata->address = htonl(ntohl(userdata->address) + 1); // Try next address
      }
      if(iter != 0){
	char temp[1024];
	fprintf(stderr,"Added address record %s (%s)\n",userdata->dns_name,inet_ntop(AF_INET,&address.data.ipv4.address,temp,sizeof(temp)));
      }
    }
    if(records) {
      // Tell the server to register the service
      int ret = avahi_entry_group_commit(userdata->group);
      if(Verbose > 1)
	fprintf(stderr,"avahi_entry_group_commit returns %d\n",ret);
      if (ret < 0){
	fprintf(stderr,"Failed to commit entry group: %s\n", avahi_strerror(ret));
	return -1;
      }
    }
  }
  return 0;
}

static void entry_group_callback(AvahiEntryGroup *g, AvahiEntryGroupState state,void *p) {
  struct userdata *userdata = (struct userdata *)p;

  assert(userdata);
  assert(g == userdata->group || userdata->group == NULL);
  userdata->group = g;
  /* Called whenever the entry group state changes */
  switch (state) {
  case AVAHI_ENTRY_GROUP_ESTABLISHED :
    if(Verbose > 1)
      fprintf(stderr,"entry_group_callback(ESTAB)\n");

    // The entry group has been established successfully
    {
      char temp[1024];
      fprintf(stderr,"service '%s.%s' -> %s (%s) established\n", userdata->service_name,
	    userdata->service_type,userdata->dns_name,
	    inet_ntop(AF_INET,&userdata->address,temp,sizeof(temp)));
    }
    pthread_mutex_lock(&userdata->avahi_mutex);
    userdata->ready = 1;
    pthread_cond_broadcast(&userdata->avahi_ready);
    pthread_mutex_unlock(&userdata->avahi_mutex);
    if(Verbose > 1)
      fprintf(stderr,"avahi_start ready condition signalled\n");
    break;
  case AVAHI_ENTRY_GROUP_COLLISION :
    if(Verbose > 1)
      fprintf(stderr,"entry_group_callback(COLLIS)\n");
#ifdef TRACE
    dump_userdata(userdata);
#endif

#if 1
    AvahiClient *c = avahi_entry_group_get_client(g);
    char *n = avahi_alternative_service_name(userdata->service_name);
    avahi_free(userdata->service_name);
    userdata->service_name = n;
    fprintf(stderr,"renaming service to %s\n",userdata->service_name);
#ifdef TRACE
    dump_userdata(userdata);
#endif
    create_services(c,userdata);
#else
    sleep(10); // Just wait in case the other guy goes away, then retry
#endif
    break;
  case AVAHI_ENTRY_GROUP_FAILURE :
    if(Verbose > 1)
      fprintf(stderr,"entry_group_callback(FAILURE)\n");
    fprintf(stderr,"Entry group failure: %s\n", avahi_strerror(avahi_client_errno(avahi_entry_group_get_client(g))));
    // Some kind of failure happened while we were registering our services
    avahi_simple_poll_quit(userdata->simple_poll);
    break;
  case AVAHI_ENTRY_GROUP_UNCOMMITED:
    if(Verbose > 1)
      fprintf(stderr,"entry_group_callback(UNCOMM)\n");
    break;
  case AVAHI_ENTRY_GROUP_REGISTERING:
    if(Verbose > 1)
      fprintf(stderr,"entry_group_callback(REGISTERING)\n");
    break;
  }
}
// Wait until the entry group has been established successfully
// This isn't necessarily a good idea; if the records have been manually created and avahi isn't running
// we'd block forever unnecessarily

// Removed the wait to avoid a deadlock when the DNS A records are already asserted elsewhere on the nt
// I *think* this will cause us to wait until the other guy goes away, and then we'll assert them ourselves
void avahi_ready(struct userdata *userdata){
#if 1
  if(Verbose > 1){
    fprintf(stderr,"NOT waiting for %p to become ready\n",userdata);
  }
#else
  if(Verbose > 1){
    fprintf(stderr,"waiting for %p to become ready\n",userdata);
    fflush(stdout);
  }
  pthread_mutex_lock(&userdata->avahi_mutex);
  while(!userdata->ready)
    pthread_cond_wait(&userdata->avahi_ready,&userdata->avahi_mutex);
  pthread_mutex_unlock(&userdata->avahi_mutex);
  if(Verbose > 1){
    fprintf(stderr,"%p is ready!\n",userdata);
    fflush(stdout);
  }
#endif
}
