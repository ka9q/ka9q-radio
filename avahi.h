#ifndef _AVAHI_H
#include <avahi-client/client.h>
#include <avahi-client/publish.h>
#include <avahi-common/alternative.h>
#include <avahi-common/simple-watch.h>
#include <avahi-common/malloc.h>
#include <avahi-common/error.h>
#include <avahi-common/timeval.h>

struct service_tab {
  void *buffer;
  char *line_type;
  char *interface;
  char *protocol;
  char *name;
  char *type;
  char *domain;
  char *dns_name;
  char *address;
  char *port;
  char *txt;
};

int avahi_browse(struct service_tab *table,int tabsize,char const *service_name);
void avahi_free_service_table(struct service_tab *table,int tabsize);

int avahi_start(char const *service_name,char const *service_type,int service_port,char const *dns_name,int base_address,char const *description,void *,int *);
#define AVAHI_H 1
#endif
