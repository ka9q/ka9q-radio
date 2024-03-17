// Keep database of _ka9q-ctl._udp (KA9Q Radio control) channels
// Modified from Avahi browser demonstration code.
// Phil Karn, March 2024

/***
  This file is part of avahi.
  avahi is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.
  avahi is distributed in the hope that it will be useful, but WITHOUT
  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
  or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General
  Public License for more details.
  You should have received a copy of the GNU Lesser General Public
  License along with avahi; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <stdbool.h>
#include <avahi-client/client.h>
#include <avahi-client/lookup.h>
#include <avahi-common/simple-watch.h>
#include <avahi-common/malloc.h>
#include <avahi-common/error.h>

#include "avahi.h"

struct avahi_db *Avahi_database;


pthread_mutex_t Avahi_browser_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t Avahi_browser_cond = PTHREAD_COND_INITIALIZER;

static AvahiSimplePoll *simple_poll = NULL;
static void resolve_callback(
    AvahiServiceResolver *r,
    AVAHI_GCC_UNUSED AvahiIfIndex interface,
    AVAHI_GCC_UNUSED AvahiProtocol protocol,
    AvahiResolverEvent event,
    const char *name,
    const char *type,
    const char *domain,
    const char *host_name,
    const AvahiAddress *address,
    uint16_t port,
    AvahiStringList *txt,
    AvahiLookupResultFlags flags,
    AVAHI_GCC_UNUSED void* userdata) {
    assert(r);
    /* Called whenever a service has been resolved successfully or timed out */
    switch (event) {
    case AVAHI_RESOLVER_FAILURE:
#if !NDEBUG
      fprintf(stderr, "(Resolver) Failed to resolve service '%s' of type '%s' in domain '%s': %s\n", name, type, domain, avahi_strerror(avahi_client_errno(avahi_service_resolver_get_client(r))));
#endif
      break;
    case AVAHI_RESOLVER_FOUND:
      {
	char *t;
	t = avahi_string_list_to_string(txt);
#if !NDEBUG
	fprintf(stderr, "Service '%s' of type '%s' in domain '%s':\n", name, type, domain);
	char a[AVAHI_ADDRESS_STR_MAX];
	avahi_address_snprint(a, sizeof(a), address);

	fprintf(stderr,
		"\t%s:%u (%s)\n"
		"\tTXT=%s\n"
		"\tcookie is %u\n"
		"\tis_local: %i\n"
		"\tour_own: %i\n"
		"\twide_area: %i\n"
		"\tmulticast: %i\n"
		"\tcached: %i\n",
		host_name, port, a,
		t,
		avahi_string_list_get_service_cookie(txt),
		!!(flags & AVAHI_LOOKUP_RESULT_LOCAL),
		!!(flags & AVAHI_LOOKUP_RESULT_OUR_OWN),
		!!(flags & AVAHI_LOOKUP_RESULT_WIDE_AREA),
		!!(flags & AVAHI_LOOKUP_RESULT_MULTICAST),
		!!(flags & AVAHI_LOOKUP_RESULT_CACHED));
#endif
	pthread_mutex_lock(&Avahi_browser_mutex);
	// Already in list?
	struct avahi_db *db;
	for(db = Avahi_database; db != NULL; db = db->next){
	  if(strcmp(db->name,name) == 0 && strcmp(db->type,type) == 0 && strcmp(db->domain,domain) == 0)
	    break;
	}
	if(db == NULL){
	  // Not already in database; add it
	  struct avahi_db *db = calloc(1,sizeof(*db));
	  db->prev = NULL; // First on list
	  db->next = Avahi_database;
	  if(db->next)
	    db->next->prev = db;
	  
	  Avahi_database = db;
	  db->name = strdup(name);
	  db->type = strdup(type);
	  db->domain = strdup(domain);
	  db->host_name = strdup(host_name);
	  db->address = *address;
	  db->port = port;
	  db->txt = strdup(t);
	  db->flags = flags;
	  pthread_cond_broadcast(&Avahi_browser_cond);
	}
	pthread_mutex_unlock(&Avahi_browser_mutex);
	avahi_free(t);
	break;
      }
    }
    avahi_service_resolver_free(r);
}
static void browse_callback(
    AvahiServiceBrowser *b,
    AvahiIfIndex interface,
    AvahiProtocol protocol,
    AvahiBrowserEvent event,
    const char *name,
    const char *type,
    const char *domain,
    AVAHI_GCC_UNUSED AvahiLookupResultFlags flags,
    void* userdata) {
    AvahiClient *c = userdata;
    assert(b);
    /* Called whenever a new services becomes available on the LAN or is removed from the LAN */
    switch (event) {
    case AVAHI_BROWSER_FAILURE:
#if !NDEBUG
      fprintf(stderr, "(Browser) %s\n", avahi_strerror(avahi_client_errno(avahi_service_browser_get_client(b))));
#endif
      avahi_simple_poll_quit(simple_poll);
      return;
    case AVAHI_BROWSER_NEW:
      fprintf(stderr, "(Browser) NEW: service '%s' of type '%s' in domain '%s'\n", name, type, domain);
      /* We ignore the returned resolver object. In the callback
	 function we free it. If the server is terminated before
	 the callback function is called the server will free
	 the resolver for us. */
      if (!(avahi_service_resolver_new(c, interface, protocol, name, type, domain, AVAHI_PROTO_UNSPEC, 0, resolve_callback, c)))
	fprintf(stderr, "Failed to resolve service '%s': %s\n", name, avahi_strerror(avahi_client_errno(c)));
      break;
    case AVAHI_BROWSER_REMOVE:
#if !NDEBUG
      fprintf(stderr, "(Browser) REMOVE: service '%s' of type '%s' in domain '%s'\n", name, type, domain);
#endif
      {
	// Remove record from our database
	pthread_mutex_lock(&Avahi_browser_mutex);
	struct avahi_db *db;
	for(db = Avahi_database; db != NULL; db = db->next){
	  if(strcmp(db->name,name) == 0 && strcmp(db->type,type) == 0 && strcmp(db->domain,domain) == 0)
	    break;
	}
	if(db != NULL){
	  if(db->next)
	    db->next->prev = db->prev;
	  if(db->prev)
	    db->prev->next = db->next;
	  else
	    Avahi_database = db->next;
	  free(db);
	  db = NULL;
	  pthread_cond_broadcast(&Avahi_browser_cond);
	}
	pthread_mutex_unlock(&Avahi_browser_mutex);
      }
      break;
    case AVAHI_BROWSER_ALL_FOR_NOW:
#if !NDEBUG
      fprintf(stderr, "(Browser) ALL_FOR_NOW\n");
#endif
      break;
    case AVAHI_BROWSER_CACHE_EXHAUSTED:
#if !NDEBUG
      fprintf(stderr, "(Browser) CACHE_EXHAUSTED\n");
#endif
      break;
    }
}
static void client_callback(AvahiClient *c, AvahiClientState state, AVAHI_GCC_UNUSED void * userdata) {
    assert(c);
    /* Called whenever the client or server state changes */
    if (state == AVAHI_CLIENT_FAILURE) {
#if !NDEBUG
      fprintf(stderr, "Server connection failure: %s\n", avahi_strerror(avahi_client_errno(c)));
#endif
      avahi_simple_poll_quit(simple_poll);
    }
}
#if 0 // testing only
int main(AVAHI_GCC_UNUSED int argc, AVAHI_GCC_UNUSED char*argv[]) {
  pthread_t av_thread;

  pthread_create(&av_thread,NULL,avahi_browser_thread,NULL);
  printf("avahi thread created");
  while(true)
    sleep(1);


}
#endif

void *avahi_browser(void *p){
    AvahiClient *client = NULL;
    AvahiServiceBrowser *sb = NULL;
    int error;
    int ret __attribute__ ((unused));  // Used only if NDEBUG not defined
    ret = 1;
    /* Allocate main loop object */
    if (!(simple_poll = avahi_simple_poll_new())) {
#if !NDEBUG
        fprintf(stderr, "Failed to create simple poll object.\n");
#endif
        goto fail;
    }
    /* Allocate a new client */
    client = avahi_client_new(avahi_simple_poll_get(simple_poll), 0, client_callback, NULL, &error);
    /* Check wether creating the client object succeeded */
    if (!client) {
#if !NDEBUG
      fprintf(stderr, "Failed to create client: %s\n", avahi_strerror(error));
#endif
      goto fail;
    }
    /* Create the service browser */
    if (!(sb = avahi_service_browser_new(client, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC, "_ka9q-ctl._udp", NULL, 0, browse_callback, client))) {
#if !NDEBUG
      fprintf(stderr, "Failed to create service browser: %s\n", avahi_strerror(avahi_client_errno(client)));
#endif
      goto fail;
    }
    // Run the main loop
    // Normally doesn't return
    avahi_simple_poll_loop(simple_poll);
    ret = 0;
 fail:
    /* Cleanup things */
    if (sb)
      avahi_service_browser_free(sb);
    if (client)
      avahi_client_free(client);
    if (simple_poll)
      avahi_simple_poll_free(simple_poll);
    pthread_mutex_destroy(&Avahi_browser_mutex);
#if !NDEBUG
    printf("avahi_thread returns %d\n",ret);
#endif
    return NULL;
}

