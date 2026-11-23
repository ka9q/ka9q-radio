// P25 messages, both phases: the decoded message, its families and kinds, one-line log
// formatting, names, and the identifier (band plan) table
// Copyright 2026, Matthew Kaufman, KA6SQG

#ifndef _P25MSG_H
#define _P25MSG_H 1

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// The Phase 1 decoders (p25p1msg.h) and the Phase 2 MAC decoder (p25p2mac.h) fill a
// struct p25_msg. Nothing allocates and nothing is global: a decoded message and a band
// plan are the caller's, so channels can be decoded on separate threads.

// Manufacturer IDs
#define P25_MFID_STD 0x00
#define P25_MFID_STD1 0x01
#define P25_MFID_MOTOROLA 0x90
#define P25_MFID_HARRIS 0xa4

// Service options octet
#define P25_SVC_EMERGENCY 0x80
#define P25_SVC_ENCRYPTED 0x40
#define P25_SVC_DUPLEX 0x20
#define P25_SVC_PACKET 0x10
#define P25_SVC_PRIORITY 0x07

// What carried a message, and so which decoder and layout it came from (the kind says what it is)
enum p25_family {
  P25_FAMILY_TSBK,
  P25_FAMILY_LC,     // link control from an LDU1
  P25_FAMILY_TDULC,  // link control from a TDULC
  P25_FAMILY_MBT,    // alternate multi-block trunking control (AMBTC)
  P25_FAMILY_PDU,    // any other packet data unit
  P25_FAMILY_HDU,
  P25_FAMILY_ES,     // LDU2 encryption sync
  P25_FAMILY_MAC,    // Phase 2 MAC structure (p25p2mac.c)
};

// What a message is, whatever family carried it (a group voice grant may come in a TSBK or
// an AMBTC). Names follow the TIA-102 abbreviations.
enum p25_kind {
  P25_UNKNOWN = 0,    // reserved or unrecognised opcode, or an MFID without a table
  // Channel grants and updates (TSBK, MBT)
  P25_GRP_V_CH_GRANT,
  P25_GRP_V_CH_GRANT_UPDT,
  P25_GRP_V_CH_GRANT_UPDT_EXP,
  P25_UU_V_CH_GRANT,
  P25_UU_ANS_REQ,
  P25_UU_V_CH_GRANT_UPDT,
  P25_TELE_INT_CH_GRANT,
  P25_TELE_INT_CH_GRANT_UPDT,
  P25_TELE_INT_ANS_REQ,
  P25_IND_DATA_CH_GRANT,
  P25_GRP_DATA_CH_GRANT,
  P25_GRP_DATA_CH_ANN,
  P25_GRP_DATA_CH_ANN_EXP,
  P25_SN_DATA_CH_GRANT,
  P25_SN_DATA_PAGE_REQ,
  P25_SN_DATA_CH_ANN_EXP,
  // Unit and network control (TSBK; several also LC)
  P25_STS_UPDT,
  P25_STS_Q,
  P25_MSG_UPDT,
  P25_RAD_MON_CMD,
  P25_CALL_ALRT,
  P25_ACK_RSP_FNE,
  P25_QUE_RSP,
  P25_EXT_FNCT_CMD,
  P25_DENY_RSP,
  P25_GRP_AFF_RSP,
  P25_GRP_AFF_Q,
  P25_LOC_REG_RSP,
  P25_U_REG_RSP,
  P25_U_REG_CMD,
  P25_AUTH_CMD,
  P25_U_DE_REG_ACK,
  P25_SYNC_BCST,
  P25_AUTH_DMAND,
  P25_AUTH_FNE_RESP,
  P25_TIME_DATE_ANN,
  P25_ROAM_ADDR_CMD,
  P25_ROAM_ADDR_UPDT,
  // System and site broadcasts (TSBK, MBT, LC)
  P25_IDEN_UP,
  P25_IDEN_UP_TDMA,
  P25_IDEN_UP_VU,
  P25_SYS_SRV_BCST,
  P25_SCCB,
  P25_SCCB_EXP,
  P25_RFSS_STS_BCST,
  P25_RFSS_STS_BCST_EXP,
  P25_NET_STS_BCST,
  P25_NET_STS_BCST_EXP,
  P25_ADJ_STS_BCST,
  P25_ADJ_STS_BCST_EXP,
  P25_ADJ_STS_BCST_UNCOORD,
  // Motorola (MFID 0x90)
  P25_MOT_GRG_ADD,
  P25_MOT_GRG_DEL,
  P25_MOT_GRG_CH_GRANT,
  P25_MOT_GRG_CH_UPDT,
  P25_MOT_EXT_FNCT_CMD,
  P25_MOT_TRAFFIC_CH_ID,
  P25_MOT_QUE_RSP,
  P25_MOT_DENY_RSP,
  P25_MOT_ACK_RSP,
  P25_MOT_SYS_LOADING,
  P25_MOT_EMERG_ALRM,
  P25_MOT_BSI,
  P25_MOT_CC_SHUTDOWN,
  P25_MOT_TDMA_DATA_CH,
  // L3Harris (MFID 0xa4)
  P25_HARRIS_GRG_EXENC_CMD,
  // Link control only (LDU1, TDULC)
  P25_LC_GRP_V_CH_USR,
  P25_LC_UU_V_CH_USR,
  P25_LC_TEL_INT_V_CH_USR,
  P25_LC_SRC_ID_EXT,
  P25_LC_UU_V_CH_USR_EXT,
  P25_LC_CALL_TRM,
  P25_LC_OTHER,               // defined in TIA-102.AABF, nothing decoded
  P25_LC_MOT_GRG_V_CH_USR,
  P25_LC_MOT_GRG_V_CH_UPDT,
  P25_LC_MOT_FAILSOFT,
  P25_LC_MOT_UNIT_GPS,
  P25_LC_MOT_TALK_COMPLETE,
  P25_LC_MOT_ALIAS_HDR,
  P25_LC_MOT_ALIAS_DATA,
  P25_LC_HARRIS_RETURN_CC,
  P25_LC_HARRIS_GPS1,
  P25_LC_HARRIS_GPS2,
  P25_LC_HARRIS_ALIAS,
  // Packet data (PDU)
  P25_PDU_RESPONSE,
  P25_PDU_SNDCP,              // SNDCP context control (SAP 6)
  P25_PDU_SNDCP_DATA,         // packet data (SAP 4)
  P25_PDU_DATA,               // other SAPs
  P25_PDU_OTHER,              // header only: UMBTC, unknown formats
  // Voice headers
  P25_HDU,
  P25_ENC_SYNC,
  P25_KIND_COUNT
};

// A channel as messages send it. On a TDMA identifier the number counts timeslots, not carriers:
// p25_chan_freq() gives the carrier and the slot
struct p25_chan {
  uint8_t iden;      // band plan identifier, 0-15
  uint16_t num;      // channel number, 0-4095
};

// A decoded message. One flat set of fields; which ones mean something depends on kind,
// as p25_format() shows. Fields a kind does not use are zero.
struct p25_msg {
  enum p25_family family;
  enum p25_kind kind;
  uint8_t opcode;          // 6-bit opcode (TSBK, LC, MBT)
  uint8_t mfid;            // manufacturer ID that chose the opcode table
  bool last_block;         // TSBK: last block of the TSDU
  bool protect;            // TSBK protected bit; LC: encrypted, only the header is readable
  bool sf;                 // LC: standard format (implicit MFID)

  // Calls and units
  uint8_t svc;             // service options
  uint16_t group;          // talkgroup, supergroup or patch group (first)
  uint16_t group_b;        // second talkgroup of a two-call update; announcement group
  uint32_t src;            // source unit (LLID for MBT)
  uint32_t tgt;            // target unit or address
  struct p25_chan ch;      // the channel; downlink of an explicit message; first of two
  struct p25_chan ch_b;    // uplink of an explicit message; second of two
  bool has_b;              // the second call/channel of an update or broadcast is present
  uint16_t timer;          // call timer, 100 ms units (telephone interconnect)
  uint16_t members[6];     // Motorola regroup member talkgroups (up to six, TIA-102.BBAD-A 3.92)
  int nmembers;

  // Responses and commands
  bool aiv;                // additional information valid
  bool ex;                 // extended information
  uint8_t svc_type;        // the service a response answers
  uint8_t reason;          // deny/queue reason
  uint32_t info;           // additional information (24 bits)
  uint16_t function;       // extended function
  uint32_t args;           // extended function arguments (24 bits)
  uint8_t response;        // registration/affiliation response
  bool local_global;       // GRP_AFF_RSP LG bit
  uint8_t unit_status;
  uint8_t user_status;
  uint16_t message;        // MSG_UPDT short message
  uint8_t tx_mult;         // RAD_MON_CMD transmit multiplier

  // System and site
  uint8_t lra;
  uint32_t wacn;           // 20 bits
  uint16_t sysid;          // 12 bits
  uint8_t rfss;
  uint8_t site;
  uint8_t svc_class;       // system service class (first channel)
  uint8_t svc_class_b;     // second channel's
  uint8_t site_flags;      // ADJ/RFSS: 8 conventional, 4 failure, 2 valid, 1 active network
  uint32_t avail_svc;      // SYS_SRV_BCST
  uint32_t supp_svc;
  uint8_t req_priority;
  uint16_t data_access;    // SN_DATA_CH_ANN_EXP data access control
  bool autonomous;         // SN_DATA_CH_ANN_EXP access flags
  bool requested;

  // Identifier updates, raw and in Hz
  uint8_t iden;
  uint8_t iden_type;       // TDMA channel type, or the VHF/UHF bandwidth code
  bool iden_tdma;
  uint8_t iden_slots;
  uint64_t base_hz;
  uint32_t spacing_hz;
  uint32_t bw_hz;
  int64_t tx_offset_hz;

  // Time (SYNC_BCST, TIME_DATE_ANN)
  uint16_t year;
  uint8_t month, day, hour, minute;
  uint16_t microslots;
  uint8_t leap;            // 2.5 ms units
  bool time_unlocked;      // system time not locked to an external reference
  bool uslot_unlocked;     // microslot-minute rollover unlocked
  bool lto_valid_bit;      // the raw bit; 0 means the offset is valid
  bool lto_negative;
  uint8_t lto_hours;
  bool lto_half;

  // Vendor
  uint16_t tch1, tch2;     // Motorola traffic channel IDs
  uint8_t load, capacity;  // Motorola system loading
  char cwid[9];            // Motorola base station ID callsign
  uint16_t shutdown_min;   // Motorola planned shutdown
  uint8_t ext_class, ext_operand;  // Motorola extended function
  uint8_t regroup_opts;    // L3Harris regroup options
  uint16_t key_id;         // L3Harris regroup key
  uint8_t alg_id;          // L3Harris regroup algorithm (talkgroup target)

  // Link control extras
  uint32_t call_timer_ms;  // telephone interconnect channel user
  uint32_t address;        // call termination, talk complete
  double latitude, longitude;  // Motorola unit GPS
  uint8_t block;           // talker alias block number
  uint8_t block_count;
  uint8_t sequence;
  uint8_t alias_format;
  char text[8];            // L3Harris talker alias fragment (7 characters)

  // Encryption sync (HDU, LDU2)
  uint8_t mi[9];
  uint8_t algid;
  uint16_t kid;
  bool zero_es;            // LDU2: sent as all zeros, taken as unencrypted (algid is 0x80 here)

  // Packet data header (PDU, MBT)
  bool confirmed;
  bool outbound;
  uint8_t format;
  uint8_t sap;
  uint32_t llid;
  uint8_t blocks;          // blocks to follow
  uint8_t pad_octets;
  uint8_t header_offset;
  uint8_t pdu_response;    // response packet class/type/status
  bool has_src_llid;
  uint32_t src_llid;
  int user_octets;         // user data length, CRC and padding removed (-1: blocks missing)
  uint8_t sndcp_type;      // SNDCP control: PDU type (high nibble of octet 0)
  uint8_t sndcp_nsapi;     // low nibble of octet 0
  bool sndcp_has_arg;
  uint8_t sndcp_arg;       // octet 1 (reason, deactivation type, NSAPI)
  bool sndcp_has_ip;
  uint8_t sndcp_ip[4];

  uint8_t raw[15];         // the first octets of the message, for unknown kinds
  int raw_len;
};

// Band plan: the 16 identifiers a site announces
struct p25_band {
  bool valid;
  bool tdma;
  uint8_t slots;           // 1 FDMA, 2 or 4 TDMA
  uint8_t type;            // TDMA channel type or VHF/UHF bandwidth code; 0 for IDEN_UP
  uint64_t base_hz;
  uint32_t spacing_hz;
  int64_t tx_offset_hz;
  uint32_t bw_hz;
};
struct p25_bandplan {
  struct p25_band band[16];
};

// One log line, no newline: family, kind, then key=value fields. Channels are printed
// as iden-number and, when the plan knows the identifier, with frequencies in MHz.
// Returns the length snprintf would have written.
int p25_format(char *buf, size_t size, struct p25_msg const *msg, struct p25_bandplan const *plan);
char const *p25_kind_name(enum p25_kind kind);
char const *p25_family_name(enum p25_family family);
char const *p25_deny_reason_name(uint8_t reason);       // "" when not defined
char const *p25_ext_function_name(uint16_t function);   // "" when not defined
char const *p25_sap_name(uint8_t sap);

// Band plan
void p25_bandplan_init(struct p25_bandplan *plan);
// Apply an identifier update (IDEN_UP, IDEN_UP_TDMA, IDEN_UP_VU from any family). Returns
// true if the plan changed; an implausible update (base outside 100-1000 MHz, spacing 0 or
// over 1 MHz, offset over 100 MHz) is ignored.
bool p25_bandplan_update(struct p25_bandplan *plan, struct p25_msg const *msg);
// Downlink frequency, the uplink frequency by the identifier's transmit offset (0 when the
// identifier has none), and the TDMA slot (0 for FDMA). False if the identifier is unknown.
bool p25_chan_freq(struct p25_bandplan const *plan, struct p25_chan ch, uint64_t *downlink_hz,
                   uint64_t *uplink_hz, int *slot);

#endif
