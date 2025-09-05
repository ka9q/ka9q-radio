// Multicast and RTP functions, constants and structures for ka9q-radio
// Copyright 2018-2023, Phil Karn, KA9Q

#ifndef _MULTICAST_H
#define _MULTICAST_H 1
#include <stdint.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <netdb.h>
#include <assert.h>

char const *formatsock(void const *, bool);
char *formataddr(char *result, int size, void const *s);

extern char const *Default_mcast_iface;

int setup_mcast(char const *source, struct sockaddr *, char const *target, struct sockaddr *, bool output, int ttl, int tos, int offset, int tries);
static inline int setup_mcast_in(char const *source, struct sockaddr *source_sock, char const *target, struct sockaddr *sock, int offset, int tries){
  return setup_mcast(source,source_sock,target,sock,0,0,0,offset,tries);
}
int join_group(int fd, struct sockaddr const * const source, struct sockaddr const * const sock,  char const * const iface);
int output_mcast(void const * const s, char const * const iface, int const ttl, int const tos);
int listen_mcast(void const *source, void const *sock, char const *iface);
int connect_mcast(void const * const s, char const * const iface, int const ttl, int const tos);
int resolve_mcast(char const *target, void *sock, int default_port, char *iface, int iface_len, int tries);
int setportnumber(void *sock, uint16_t port);
int getportnumber(void const *sock);
int address_match(void const *arg1, void const *arg2);

void dump_interfaces(void);

// Utility routines for reading from, and writing integers to, network format in char buffers
static inline uint8_t get8(uint8_t const *dp){
  assert(dp != NULL);
  return *dp;
}

static inline uint16_t get16(uint8_t const *dp){
  assert(dp != NULL);
  return dp[0] << 8 | dp[1];
}

static inline uint32_t get24(uint8_t const *dp){
  assert(dp != NULL);
  return dp[0] << 16 | dp[1] << 8 | dp[2];
}

static inline uint32_t get32(uint8_t const *dp){
  assert(dp != NULL);
  return dp[0] << 24 | dp[1] << 16 | dp[2] << 8 | dp[3];
}

static inline uint8_t *put8(uint8_t *dp,uint8_t x){
  assert(dp != NULL);
  *dp++ = x;
  return dp;
}

static inline uint8_t *put16(uint8_t *dp,uint16_t x){
  assert(dp != NULL);
  *dp++ = x >> 8;
  *dp++ = x;
  return dp;
}

static inline uint8_t *put24(uint8_t *dp,uint32_t x){
  assert(dp != NULL);
  *dp++ = x >> 16;
  *dp++ = x >> 8;
  *dp++ = x;
  return dp;
}

static inline uint8_t *put32(uint8_t *dp,uint32_t x){
  assert(dp != NULL);
  *dp++ = x >> 24;
  *dp++ = x >> 16;
  *dp++ = x >> 8;
  *dp++ = x;
  return dp;
}
int samprate_from_pt(int type);
int channels_from_pt(int type);
enum encoding encoding_from_pt(int type);
int pt_from_info(unsigned int samprate,unsigned int channels,enum encoding);
char const *encoding_string(enum encoding);
enum encoding parse_encoding(char const *str);
uint32_t make_maddr(char const *arg);
int setport(void *sock,int port);

#endif
