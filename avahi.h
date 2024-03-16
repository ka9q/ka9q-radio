#ifndef _AVAHI_H
#include <avahi-client/client.h>
#include <avahi-client/publish.h>
#include <avahi-common/alternative.h>
#include <avahi-common/simple-watch.h>
#include <avahi-common/malloc.h>
#include <avahi-common/error.h>
#include <avahi-common/timeval.h>

void *avahi_browser_thread(void *p);
struct db {
  struct db *next;
  struct db *prev;
  char *name;
  char *type;
  char *domain;
  char *host_name;
  AvahiAddress address;
  uint16_t port;
  char *txt;
  AvahiLookupResultFlags flags;
};
extern struct db *Avahi_database;


void *avahi_browser_thread(void *);
int avahi_start(char const *service_name,char const *service_type,int service_port,char const *dns_name,int base_address,char const *description,void *,int *);
#define AVAHI_H 1
#endif
