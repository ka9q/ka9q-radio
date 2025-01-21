// Multicast and RTP functions, constants and structures for ka9q-radio
// Copyright 2018-2023, Phil Karn, KA9Q

#ifndef _MULTICAST_H
#define _MULTICAST_H 1
#include <stdint.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <netdb.h>
#include <assert.h>

#define DEFAULT_MCAST_PORT (5004)
#define DEFAULT_RTP_PORT (5004)
#define DEFAULT_RTCP_PORT (5005)
#define DEFAULT_STAT_PORT (5006)

#define NTP_EPOCH 2208988800UL // Seconds between Jan 1 1900 and Jan 1 1970

#define RTP_MIN_SIZE 12  // min size of RTP header
#define RTP_VERS 2U
#define RTP_MARKER 0x80  // Marker flag in mpt field

extern int Mcast_ttl;
extern int IP_tos;

enum encoding {
  NO_ENCODING = 0,
  S16LE,
  S16BE,
  OPUS,
  F32LE,
  AX25,
  F16LE,
  UNUSED_ENCODING, // Sentinel, not used
};

struct pt_table {
  unsigned int samprate;
  unsigned int channels;
  enum encoding encoding;
};

extern struct pt_table PT_table[];
extern int const Opus_pt; // Allow dynamic setting in the future
extern int const AX25_pt;

// Internal representation of RTP header -- NOT what's on wire!
struct rtp_header {
  int version;
  uint8_t type;
  uint16_t seq;
  uint32_t timestamp;
  uint32_t ssrc;
  bool marker:1;
  bool pad:1;
  bool extension:1;
  int cc;
  uint32_t csrc[15];
};

// RTP sender/receiver state
struct rtp_state {
  uint32_t ssrc;
  uint8_t type;
  bool init;
  uint16_t seq;
  uint16_t odd_seq;
  bool odd_seq_set;
  uint32_t timestamp;
  uint64_t packets;
  uint64_t bytes;
  uint64_t drops;
  uint64_t dupes;
};

// Internal format of sender report segment
struct rtcp_sr {
  uint32_t ssrc;
  int64_t ntp_timestamp;
  unsigned int rtp_timestamp;
  unsigned int packet_count;
  unsigned int byte_count;
};

// Internal format of receiver report segment
struct rtcp_rr {
  uint32_t ssrc;
  int lost_fract;
  int lost_packets;
  int highest_seq;
  int jitter;
  int lsr; // Last SR
  int dlsr; // Delay since last SR
};

// Internal format of RTCP source description
enum sdes_type {
  CNAME=1,
  NAME=2,
  EMAIL=3,
  PHONE=4,
  LOC=5,
  TOOL=6,
  NOTE=7,
  PRIV=8,
};

// Individual source description item
struct rtcp_sdes {
  enum sdes_type type;
  uint32_t ssrc;
  int mlen;
  char message[256];
};

char const *formatsock(void const *,bool);
char *formataddr(char *result,int size,void const *s);

#define PKTSIZE 65536 // Largest possible IP datagram, in case we use jumbograms
// Incoming RTP packets
// This should probably be extracted into a more general RTP library
struct packet {
  struct packet *next;
  struct rtp_header rtp;
  uint8_t const *data; // Don't modify a packet through this pointer
  size_t len;
  uint8_t content[PKTSIZE];
};



// Convert between internal and wire representations of RTP header
void const *ntoh_rtp(struct rtp_header *,void const *);
void *hton_rtp(void *, struct rtp_header const *);

extern char const *Default_mcast_iface;

int setup_mcast(char const *target,struct sockaddr *,int output,int ttl,int tos,int offset,int tries);
static inline int setup_mcast_in(char const *target,struct sockaddr *sock,int offset,int tries){
  return setup_mcast(target,sock,0,0,0,offset,tries);
}
int setup_loopback(int);
int join_group(int fd,struct sockaddr const * const sock, char const * const iface,int const ttl,int const tos);
int connect_mcast(void const *sock,char const *iface,int const ttl,int const tos);
int listen_mcast(void const *sock,char const *iface);
int resolve_mcast(char const *target,void *sock,int default_port,char *iface,int iface_len,int tries);
int setportnumber(void *sock,uint16_t port);
int getportnumber(void const *sock);
int address_match(void const *arg1,void const *arg2);
int add_pt(int type, unsigned int samprate, unsigned int channels, enum encoding encoding);

// Function to process incoming RTP packet headers
// Returns number of samples dropped or skipped by silence suppression, if any
int rtp_process(struct rtp_state *state,struct rtp_header const *rtp,int samples);

// Generate RTCP source description segment
uint8_t *gen_sdes(uint8_t *output,int bufsize,uint32_t ssrc,struct rtcp_sdes const *sdes,int sc);
// Generate RTCP bye segment
uint8_t *gen_bye(uint8_t *output,int bufsize,uint32_t const *ssrcs,int sc);
// Generate RTCP sender report segment
uint8_t *gen_sr(uint8_t *output,int bufsize,struct rtcp_sr const *sr,struct rtcp_rr const *rr,int rc);
// Generate RTCP receiver report segment
uint8_t *gen_rr(uint8_t *output,int bufsize,uint32_t ssrc,struct rtcp_rr const *rr,int rc);

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

#endif
