#ifndef _RTP_H
#define _RTP_H 1

#define DEFAULT_MCAST_PORT ((uint16_t)5004)
#define DEFAULT_RTP_PORT ((uint16_t)5004)
#define DEFAULT_RTCP_PORT ((uint16_t)5005)
#define DEFAULT_STAT_PORT ((uint16_t)5006)

#define NTP_EPOCH 2208988800UL // Seconds between Jan 1 1900 and Jan 1 1970

#define RTP_MIN_SIZE 12  // min size of RTP header
#define RTP_VERS 2U
#define RTP_MARKER 0x80  // Marker flag in mpt field

#define OPUS_SAMPRATE (48000)
#define PKTSIZE 65536 // Largest possible IP datagram, in case we use jumbograms

struct string_table {
  char *str;
  int value;
};

enum encoding {
  NO_ENCODING = 0,
  S16LE,
  S16BE,
  OPUS,
  F32LE,
  AX25,
  F16LE,
  OPUS_VOIP,       // Opus with APPLICATION_VOIP
  UNUSED_ENCODING, // Sentinel, not used
};

struct pt_table {
  int samprate;
  int channels;
  enum encoding encoding;
};

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
  size_t mlen;
  char message[256];
};

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

int add_pt(int type, int samprate, int channels, enum encoding encoding);
// Function to process incoming RTP packet headers
// Returns number of samples dropped or skipped by silence suppression, if any
int rtp_process(struct rtp_state *state,struct rtp_header const *rtp,size_t samples);

// Generate RTCP source description segment
uint8_t *gen_sdes(uint8_t *output,size_t bufsize,uint32_t ssrc,struct rtcp_sdes const *sdes,int sc);
// Generate RTCP bye segment
uint8_t *gen_bye(uint8_t *output,size_t bufsize,uint32_t const *ssrcs,int sc);
// Generate RTCP sender report segment
uint8_t *gen_sr(uint8_t *output,size_t bufsize,struct rtcp_sr const *sr,struct rtcp_rr const *rr,int rc);
// Generate RTCP receiver report segment
uint8_t *gen_rr(uint8_t *output,size_t bufsize,uint32_t ssrc,struct rtcp_rr const *rr,int rc);

extern struct pt_table PT_table[];
extern int const Opus_pt; // Allow dynamic setting in the future
extern int const AX25_pt;

// Allowable Opus block durations, millisec * 10
extern int Opus_blocksizes[];
extern int Opus_samprates[];
// Codec helpers
extern struct string_table Opus_application[];
extern struct string_table Opus_signal[];

int opus_bandwidth(char const **str,int code);
char const *opus_application_string(int);
int opus_bandwidth_to_code(int bw);
bool legal_opus_size(int n);
bool legal_opus_samprate(int n);

int samprate_from_pt(int type);
int channels_from_pt(int type);
enum encoding encoding_from_pt(int type);
int pt_from_info(int samprate,int channels,enum encoding);
char const *encoding_string(enum encoding);
enum encoding parse_encoding(char const *str);

#endif
