#ifndef _AVAHI_H
#include <avahi-client/client.h>
#include <avahi-client/publish.h>
#include <avahi-common/alternative.h>
#include <avahi-common/simple-watch.h>
#include <avahi-common/malloc.h>
#include <avahi-common/error.h>
#include <avahi-common/timeval.h>

struct avahi_db {
  struct avahi_db *next;
  struct avahi_db *prev;
  char *name;
  char *type;
  char *domain;
  char *host_name;
  AvahiAddress address;
  uint16_t port;
  char *txt;
  AvahiLookupResultFlags flags;
};
extern struct avahi_db *Avahi_database;
extern pthread_mutex_t Avahi_browser_mutex;
extern pthread_cond_t Avahi_browser_cond;


void *avahi_browser(void *);
int avahi_start(char const *service_name,char const *service_type,int service_port,char const *dns_name,int base_address,char const *description,void *,int *);
#define AVAHI_H 1
#endif
