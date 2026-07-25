#ifndef AVAHI_H
#define AVAHI_H 1

// Provider-neutral service-discovery interface.
// Implemented by avahi.c/avahi_browse.c (Linux, via avahi-* CLI tools) or by
// mdns.c (dns_sd.h / Bonjour API, portable to macOS and FreeBSD). The build
// system selects exactly one implementation; consumers include only this header.

#include <stdbool.h>

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
