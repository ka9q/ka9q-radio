// P25 Phase 2 MAC PDU parsing (TIA-102.BBAC, BBAD-A): PTT and END PTT, and the MAC structures
// of a SIGNAL, IDLE, ACTIVE or HANGTIME PDU, decoded into struct p25_msg
// Copyright 2026, Matthew Kaufman, KA6SQG

#ifndef _P25P2MAC_H
#define _P25P2MAC_H 1

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "p25msg.h"

// The input is the PDU after its Reed-Solomon decode and CRC check, as octets, most
// significant bit first, CRC included: 180 bits from a SACCH or LCCH, 156 from a FACCH.
// Structures are read up to the CRC (bit 168 SACCH, 144 FACCH, 152 LCCH). Bits past nbits
// read as zero, and no structure starts past them. Nothing allocates and nothing is global.

enum p25_mac_chan {
  P25_MAC_SACCH,
  P25_MAC_FACCH,
  P25_MAC_LCCH,
};

// MAC PDU types, bits 0-2 (TIA-102.BBAD-A 4.1)
#define P25_MAC_SIGNAL 0
#define P25_MAC_PTT 1
#define P25_MAC_END_PTT 2
#define P25_MAC_IDLE 3
#define P25_MAC_ACTIVE 4
#define P25_MAC_HANGTIME 6

#define P25_ALGID_CLEAR 0x80

// What a structure is when p25msg has no kind for it. P25_MAC_MSG: msg.kind says.
enum p25_mac_kind {
  P25_MAC_MSG = 0,
  P25_MAC_NULL,                // 0x00 null information
  P25_MAC_NULL_AZB,            // 0x08 null avoid zero bias
  P25_MAC_MULTI_FRAG,          // 0x10 multi-fragment continuation
  P25_MAC_GRP_PAGING,          // 0x11 indirect group paging without priority
  P25_MAC_IND_PAGING,          // 0x12 individual paging with priority
  P25_MAC_GRP_V_CH_GRANT_UPDT_MULT,      // 0x05 up to three implicit updates
  P25_MAC_GRP_V_CH_GRANT_UPDT_MULT_EXP,  // 0x25 up to two explicit updates
  P25_MAC_PWR_CTL,             // 0x30 power control signal quality
  P25_MAC_RELEASE,             // 0x31 MAC release (call preemption)
  P25_MAC_GRG_V_CH_USR,        // 0x90 group regroup voice channel user, standard vendor
  P25_MAC_HARRIS_DATA_GRANT,   // L3Harris 0xa0 private data channel grant, 0xac unit-to-unit
  P25_MAC_HARRIS_ALIAS,        // L3Harris 0xa8 talker alias (whole, in one structure)
  P25_MAC_OTHER,               // defined, length known, fields not decoded
  P25_MAC_KIND_COUNT
};

struct p25_mac_grant {
  uint8_t svc;
  struct p25_chan ch;          // downlink (transmit)
  struct p25_chan ch_ul;       // uplink (receive), explicit forms only
  uint16_t group;
};

#define P25_MAC_MAX_RAW 24
// Messages a PDU can hold: 20 octets of a SACCH PDU after its header, and no message shorter
// than 2 octets (opcode and length octet)
#define P25_MAC_MAX_STRUCT 10

// One MAC structure. msg holds what the Phase 1 messages also have, so the trunking logic and
// p25_format() take it as they take a TSBK; the fields after it are those only Phase 2 has, for
// the log (and the grant list for p25_mac_trunk_msgs()).
struct p25_mac_struct {
  uint8_t opcode;              // 8 bits: partition (B1 B2) and opcode
  uint8_t vendor;              // manufacturer of a vendor-partition structure, else 0
  int offset;                  // bit index of the opcode in the PDU
  int len;                     // octets, opcode included
  enum p25_mac_kind mac_kind;
  struct p25_msg msg;          // the fields that fit p25msg; family P25_FAMILY_MAC

  // Fields p25_msg has no room for
  bool has_nac;
  uint16_t nac;                // color code: NET_STS_BCST, MAC_RELEASE
  bool has_suid;
  uint32_t suid;               // unit ID of the source SUID (msg.wacn, msg.sysid hold the rest)
  int ngrants;
  struct p25_mac_grant grant[3];   // grant updates (0x05, 0x25, 0x42, 0xc3)
  int nmembers;
  uint16_t members[6];         // Motorola regroup add/delete, L3Harris regroup talkgroups
  bool forced, audio;          // MAC release: unforced/forced, call audio
  uint8_t rf_level, ber;       // power control
  int npaged;
  uint32_t paged[4];           // paging: groups (0x11) or units (0x12)
  uint8_t second;              // TIME_DATE_ANN
  char digits[11];             // TELE_INT_ANS_REQ dialled digits
  char alias[P25_MAC_MAX_RAW]; // L3Harris talker alias, NUL terminated, raw octets
  int frag_bits;               // Motorola talker alias fragment: header 64 bits (72-135),
  uint8_t frag[13];            // data block 100 bits (36-135), msb first

  uint8_t raw[P25_MAC_MAX_RAW];  // the structure's octets, as many as the PDU holds
  int raw_len;
};

// One PDU: the header fields, PTT or END PTT fields, or the structures (SIGNAL, IDLE, ACTIVE,
// HANGTIME). At most three structures are kept, as p25_mac_decode() walks them.
struct p25_mac_pdu {
  enum p25_mac_chan chan;
  uint8_t type;                // P25_MAC_PTT ...
  uint8_t offset_4v;           // offset to the next 4V burst, bits 3-5 (TIA-102.BBAD-A 4.2)
  bool protect;                // bit 7, protected: p25_mac_decode() then returns -1 (BBAD-A 4.3)
  uint16_t lcch_nac;           // LCCH: color code, bits 152-163 (TIA-102.BBAD-A 2.1)

  // PTT: MI 8-79, ALGID 80-87, KID 88-103, source 104-127, group 128-143
  // END PTT: color code 12-23, source, group (TIA-102.BBAD-A 2.3.3.1, 2.3.3.2)
  uint8_t mi[9];
  uint8_t algid;               // P25_ALGID_CLEAR unless a PTT says otherwise
  uint16_t kid;
  uint32_t src;
  uint16_t group;
  uint16_t nac;                // END PTT color code

  int nstruct;
  struct p25_mac_struct s[P25_MAC_MAX_STRUCT];
};

// Parse a PDU. Returns the number of structures, or -1 if pdu is NULL, nbits < 8, or the PDU
// is protected (TIA-102.BBAD-A 4.3: discarded by a receiver without the protection).
int p25_mac_decode(struct p25_mac_pdu *pdu, enum p25_mac_chan chan, uint8_t const *octets, int nbits);

// Structure length rule alone: octets of the structure at bit offset, opcode included, as
// p25_mac_decode walks them (the rest of the PDU for an unknown opcode); 0 ends the walk
int p25_mac_struct_len(uint8_t const *octets, int nbits, enum p25_mac_chan chan, int offset);

// The messages a structure stands for, in the form p25_trunk_control() takes (P25_FAMILY_MAC):
// a structure p25msg has a kind for as it is; a multiple group grant update (0x05, 0x25) as its
// single updates (implicit P25_GRP_V_CH_GRANT_UPDT with the group in 'group', explicit
// P25_GRP_V_CH_GRANT_UPDT_EXP), each with its own service options; anything else none.
// Returns how many (0-3) were written to out[]
int p25_mac_trunk_msgs(struct p25_mac_struct const *s, struct p25_msg out[3]);

// Multi-fragment messages (TIA-102.BBAD-A 3: 0x71, 0xc7, 0xcb, 0xcd, 0xce, 0xcf, 0xd9, 0xdb,
// 0xde, 0xe0): a first fragment and continuations (0x10) in the LCCH bursts that follow (both
// timeslots' on a dual LCCH). One struct p25_mac_frag per LCCH, zeroed (or p25_mac_frag_reset())
// to begin. Feed it every LCCH PDU that passed its CRC, in order, and reset it at an LCCH burst
// that did not (a continuation may have been lost); a PDU without a fragment, another message, a
// new first fragment, a length that cannot be right or a Multi-Fragment CRC-16 that fails also
// drops what is pending, so a half message never lingers. True when this PDU completed one: *out
// is it, parsed into the same kinds as the single-PDU forms (offset 0, len its octets), ready for
// p25_mac_trunk_msgs() and p25_mac_format()
#define P25_MAC_FRAG_MAX 64          // octets of a reassembled message
struct p25_mac_frag {
  bool active;
  uint8_t opcode;
  int need;                          // data octets the first fragment announced
  int have;                          // data octets received
  int len;                           // octets in buf: the message as one structure
  uint8_t buf[P25_MAC_FRAG_MAX];
};
bool p25_mac_multi_frag(uint8_t opcode);
void p25_mac_frag_reset(struct p25_mac_frag *f);
bool p25_mac_frag_add(struct p25_mac_frag *f, struct p25_mac_pdu const *pdu, struct p25_mac_struct *out);

// One log line, no newline. index -1 (or a PTT / END PTT PDU) is the PDU itself; 0..nstruct-1
// a structure: "MAC <kind> chan=<sacch|facch|lcch> pdu=<type>" then key=value fields as
// p25_format() writes them. Returns the length snprintf would have written.
int p25_mac_format(char *buf, size_t size, struct p25_mac_pdu const *pdu, int index,
                   struct p25_bandplan const *plan);
char const *p25_mac_kind_name(struct p25_mac_struct const *s);
char const *p25_mac_pdu_name(uint8_t type);
char const *p25_mac_chan_name(enum p25_mac_chan chan);

#endif
