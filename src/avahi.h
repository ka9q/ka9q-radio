#ifndef AVAHI_H
#define AVAHI_H 1

// No Avahi library headers are needed here. avahi.c and avahi_browse.c
// implement this interface by running the avahi-* command line tools, so no
// Avahi type or symbol appears in either the interface or the implementation.
#include <stdbool.h> // for Static_avahi

extern bool Static_avahi;

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

int avahi_start(char const *service_name,char const *service_type,int service_port,char const *dns_name,int base_address,char const *description);

#endif
