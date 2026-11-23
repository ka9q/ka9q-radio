// P25 messages, both phases: the identifier (band plan) table, kind and field names, and
// one-line log formatting of a decoded message. The Phase 1 decoders are in p25p1msg.c,
// the Phase 2 MAC decoder in p25p2mac.c
// Copyright 2026, Matthew Kaufman, KA6SQG

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "p25msg.h"

// Theory of operation
//
// One message model for both phases. Every decoder (TSBK, link control, multi-block trunking,
// packet data header, HDU and encryption sync in p25p1msg.c; MAC structures in p25p2mac.c)
// zeroes a struct p25_msg and fills it. family says what carried the message and kind what it
// is, so a group voice grant is P25_GRP_V_CH_GRANT whether it came in a TSBK, an AMBTC or a
// MAC structure, and the call tracking (p25trunk.c) and the formatter below handle each kind
// once, not once per carrier. The struct is one flat set of fields rather than a layout per
// kind: a kind uses the fields p25_format() prints for it, and the rest stay zero, so a field
// a kind does not set reads 0, never something left from another message. The kind comes from
// the opcode and the MFID together: both standard MFIDs (0x00, 0x01) have the TIA-102 tables,
// Motorola (0x90) and L3Harris (0xa4) their own, and another manufacturer's message is
// P25_UNKNOWN, logged with its raw octets.
//
// The band plan. A channel is sent as a 4-bit identifier and a 12-bit number. The site's
// identifier updates (IDEN_UP, IDEN_UP_VU, IDEN_UP_TDMA) give each identifier a base
// frequency, a channel spacing, a transmit offset and, for TDMA, the timeslots per carrier.
// The downlink frequency is base + spacing x carrier, where the carrier is the channel number,
// or on a TDMA identifier the number divided by the slots per carrier, the remainder being the
// timeslot: a TDMA channel number counts timeslots. Until the site has sent an identifier,
// channels on it have no frequency (p25_chan_freq() is false), and the call tracking does not
// take a grant on it. An update is kept only within the limits p25_bandplan_update() checks,
// so that a single implausible update cannot move every later grant on that identifier to a
// frequency no receiver could tune.
//
// The log line. One line per message: family, kind, then key=value fields, with channels as
// identifier-number and, once the plan has the identifier, frequencies in MHz (and the slot).
// Each kind's case prints the fields its decoders set, which differ a little between families
// (a TSBK unit-to-unit grant has no service options, a multi-block message no LRA): the checks
// on family in the cases below are those differences.

// Band plan

void p25_bandplan_init(struct p25_bandplan *plan){
  memset(plan, 0, sizeof *plan);
}

bool p25_bandplan_update(struct p25_bandplan *plan, struct p25_msg const *msg){
  if(msg->kind != P25_IDEN_UP && msg->kind != P25_IDEN_UP_TDMA && msg->kind != P25_IDEN_UP_VU)
    return false;
  int64_t const off = msg->tx_offset_hz < 0 ? -msg->tx_offset_hz : msg->tx_offset_hz;
  // A plan that makes no sense is not taken: a base outside 25 MHz to 1 GHz (the land mobile
  // bands, VHF low band included), no channel spacing or more than 1 MHz, or a transmit offset
  // of more than 100 MHz
  if(msg->base_hz < 25000000 || msg->base_hz > 1000000000 || msg->spacing_hz == 0
     || msg->spacing_hz > 1000000 || off > 100000000)
    return false;
  // Only IDEN_UP_TDMA can make an identifier TDMA, and only with a TDMA channel type; an
  // IDEN_UP_TDMA of an FDMA type is a one-slot channel like the others
  struct p25_band b = {
    .valid = true,
    .tdma = msg->kind == P25_IDEN_UP_TDMA && msg->iden_tdma,
    .slots = msg->kind == P25_IDEN_UP_TDMA ? msg->iden_slots : 1,
    .type = msg->kind == P25_IDEN_UP ? 0 : msg->iden_type,
    .base_hz = msg->base_hz,
    .spacing_hz = msg->spacing_hz,
    .tx_offset_hz = msg->tx_offset_hz,
    .bw_hz = msg->bw_hz,
  };
  struct p25_band *old = &plan->band[msg->iden & 15];
  bool const changed = memcmp(old, &b, sizeof b) != 0;
  *old = b;
  return changed;
}

bool p25_chan_freq(struct p25_bandplan const *plan, struct p25_chan ch, uint64_t *downlink_hz,
                   uint64_t *uplink_hz, int *slot){
  if(plan == NULL || ch.iden > 15 || !plan->band[ch.iden].valid)
    return false;
  struct p25_band const *b = &plan->band[ch.iden];
  // A TDMA channel number counts slots: channel / slots is the carrier, the remainder the slot
  int const slots = (b->tdma && b->slots > 1) ? b->slots : 1;
  uint64_t const dl = b->base_hz + (uint64_t)b->spacing_hz * (ch.num / slots);
  if(downlink_hz)
    *downlink_hz = dl;
  // A negative offset (the usual one below the downlink) wraps through uint64_t to the right sum
  if(uplink_hz)
    *uplink_hz = b->tx_offset_hz == 0 ? 0 : dl + (uint64_t)b->tx_offset_hz;
  if(slot)
    *slot = ch.num % slots;
  return true;
}

// Names

static char const *const Kind_names[P25_KIND_COUNT] = {
  [P25_UNKNOWN] = "UNKNOWN",
  [P25_GRP_V_CH_GRANT] = "GRP_V_CH_GRANT",
  [P25_GRP_V_CH_GRANT_UPDT] = "GRP_V_CH_GRANT_UPDT",
  [P25_GRP_V_CH_GRANT_UPDT_EXP] = "GRP_V_CH_GRANT_UPDT_EXP",
  [P25_UU_V_CH_GRANT] = "UU_V_CH_GRANT",
  [P25_UU_ANS_REQ] = "UU_ANS_REQ",
  [P25_UU_V_CH_GRANT_UPDT] = "UU_V_CH_GRANT_UPDT",
  [P25_TELE_INT_CH_GRANT] = "TELE_INT_CH_GRANT",
  [P25_TELE_INT_CH_GRANT_UPDT] = "TELE_INT_CH_GRANT_UPDT",
  [P25_TELE_INT_ANS_REQ] = "TELE_INT_ANS_REQ",
  [P25_IND_DATA_CH_GRANT] = "IND_DATA_CH_GRANT",
  [P25_GRP_DATA_CH_GRANT] = "GRP_DATA_CH_GRANT",
  [P25_GRP_DATA_CH_ANN] = "GRP_DATA_CH_ANN",
  [P25_GRP_DATA_CH_ANN_EXP] = "GRP_DATA_CH_ANN_EXP",
  [P25_SN_DATA_CH_GRANT] = "SN_DATA_CH_GRANT",
  [P25_SN_DATA_PAGE_REQ] = "SN_DATA_PAGE_REQ",
  [P25_SN_DATA_CH_ANN_EXP] = "SN_DATA_CH_ANN_EXP",
  [P25_STS_UPDT] = "STS_UPDT",
  [P25_STS_Q] = "STS_Q",
  [P25_MSG_UPDT] = "MSG_UPDT",
  [P25_RAD_MON_CMD] = "RAD_MON_CMD",
  [P25_CALL_ALRT] = "CALL_ALRT",
  [P25_ACK_RSP_FNE] = "ACK_RSP_FNE",
  [P25_QUE_RSP] = "QUE_RSP",
  [P25_EXT_FNCT_CMD] = "EXT_FNCT_CMD",
  [P25_DENY_RSP] = "DENY_RSP",
  [P25_GRP_AFF_RSP] = "GRP_AFF_RSP",
  [P25_GRP_AFF_Q] = "GRP_AFF_Q",
  [P25_LOC_REG_RSP] = "LOC_REG_RSP",
  [P25_U_REG_RSP] = "U_REG_RSP",
  [P25_U_REG_CMD] = "U_REG_CMD",
  [P25_AUTH_CMD] = "AUTH_CMD",
  [P25_U_DE_REG_ACK] = "U_DE_REG_ACK",
  [P25_SYNC_BCST] = "SYNC_BCST",
  [P25_AUTH_DMAND] = "AUTH_DMAND",
  [P25_AUTH_FNE_RESP] = "AUTH_FNE_RESP",
  [P25_TIME_DATE_ANN] = "TIME_DATE_ANN",
  [P25_ROAM_ADDR_CMD] = "ROAM_ADDR_CMD",
  [P25_ROAM_ADDR_UPDT] = "ROAM_ADDR_UPDT",
  [P25_IDEN_UP] = "IDEN_UP",
  [P25_IDEN_UP_TDMA] = "IDEN_UP_TDMA",
  [P25_IDEN_UP_VU] = "IDEN_UP_VU",
  [P25_SYS_SRV_BCST] = "SYS_SRV_BCST",
  [P25_SCCB] = "SCCB",
  [P25_SCCB_EXP] = "SCCB_EXP",
  [P25_RFSS_STS_BCST] = "RFSS_STS_BCST",
  [P25_RFSS_STS_BCST_EXP] = "RFSS_STS_BCST_EXP",
  [P25_NET_STS_BCST] = "NET_STS_BCST",
  [P25_NET_STS_BCST_EXP] = "NET_STS_BCST_EXP",
  [P25_ADJ_STS_BCST] = "ADJ_STS_BCST",
  [P25_ADJ_STS_BCST_EXP] = "ADJ_STS_BCST_EXP",
  [P25_ADJ_STS_BCST_UNCOORD] = "ADJ_STS_BCST_UNCOORD",
  [P25_MOT_GRG_ADD] = "MOT_GRG_ADD",
  [P25_MOT_GRG_DEL] = "MOT_GRG_DEL",
  [P25_MOT_GRG_CH_GRANT] = "MOT_GRG_CH_GRANT",
  [P25_MOT_GRG_CH_UPDT] = "MOT_GRG_CH_UPDT",
  [P25_MOT_EXT_FNCT_CMD] = "MOT_EXT_FNCT_CMD",
  [P25_MOT_TRAFFIC_CH_ID] = "MOT_TRAFFIC_CH_ID",
  [P25_MOT_QUE_RSP] = "MOT_QUE_RSP",
  [P25_MOT_DENY_RSP] = "MOT_DENY_RSP",
  [P25_MOT_ACK_RSP] = "MOT_ACK_RSP",
  [P25_MOT_SYS_LOADING] = "MOT_SYS_LOADING",
  [P25_MOT_EMERG_ALRM] = "MOT_EMERG_ALRM",
  [P25_MOT_BSI] = "MOT_BSI",
  [P25_MOT_CC_SHUTDOWN] = "MOT_CC_SHUTDOWN",
  [P25_MOT_TDMA_DATA_CH] = "MOT_TDMA_DATA_CH",
  [P25_HARRIS_GRG_EXENC_CMD] = "HARRIS_GRG_EXENC_CMD",
  [P25_LC_GRP_V_CH_USR] = "GRP_V_CH_USR",
  [P25_LC_UU_V_CH_USR] = "UU_V_CH_USR",
  [P25_LC_TEL_INT_V_CH_USR] = "TEL_INT_V_CH_USR",
  [P25_LC_SRC_ID_EXT] = "SRC_ID_EXT",
  [P25_LC_UU_V_CH_USR_EXT] = "UU_V_CH_USR_EXT",
  [P25_LC_CALL_TRM] = "CALL_TRM",
  [P25_LC_OTHER] = "LC_OTHER",
  [P25_LC_MOT_GRG_V_CH_USR] = "MOT_GRG_V_CH_USR",
  [P25_LC_MOT_GRG_V_CH_UPDT] = "MOT_GRG_V_CH_UPDT",
  [P25_LC_MOT_FAILSOFT] = "MOT_FAILSOFT",
  [P25_LC_MOT_UNIT_GPS] = "MOT_UNIT_GPS",
  [P25_LC_MOT_TALK_COMPLETE] = "MOT_TALK_COMPLETE",
  [P25_LC_MOT_ALIAS_HDR] = "MOT_ALIAS_HDR",
  [P25_LC_MOT_ALIAS_DATA] = "MOT_ALIAS_DATA",
  [P25_LC_HARRIS_RETURN_CC] = "HARRIS_RETURN_CC",
  [P25_LC_HARRIS_GPS1] = "HARRIS_GPS1",
  [P25_LC_HARRIS_GPS2] = "HARRIS_GPS2",
  [P25_LC_HARRIS_ALIAS] = "HARRIS_ALIAS",
  [P25_PDU_RESPONSE] = "PDU_RESPONSE",
  [P25_PDU_SNDCP] = "SNDCP_CONTROL",
  [P25_PDU_SNDCP_DATA] = "SNDCP_DATA",
  [P25_PDU_DATA] = "PACKET_DATA",
  [P25_PDU_OTHER] = "PDU",
  [P25_HDU] = "VOICE_HDR",
  [P25_ENC_SYNC] = "ENC_SYNC",
};

char const *p25_kind_name(enum p25_kind kind){
  if(kind < 0 || kind >= P25_KIND_COUNT || Kind_names[kind] == NULL)
    return "?";
  return Kind_names[kind];
}

char const *p25_family_name(enum p25_family family){
  static char const *const names[] = { "TSBK", "LC", "TDULC", "MBT", "PDU", "HDU", "ES", "MAC" };
  if(family < 0 || family >= (int)(sizeof names / sizeof names[0]))
    return "?";
  return names[family];
}

// TIA-102.AABC deny and queued response reasons
char const *p25_deny_reason_name(uint8_t reason){
  switch(reason){
  case 0x10: return "requester_invalid";
  case 0x11: return "requester_unauthorized";
  case 0x20: return "target_invalid";
  case 0x21: return "target_unauthorized";
  case 0x2f: return "target_refused";
  case 0x30: return "group_invalid";
  case 0x31: return "group_unauthorized";
  case 0x40: return "invalid_dialing";
  case 0x41: return "number_unauthorized";
  case 0x42: return "pstn_invalid";
  case 0x50: return "call_timeout";
  case 0x51: return "landline_terminated";
  case 0x52: return "unit_terminated";
  case 0x5f: return "call_preempted";
  case 0x60: return "site_access_denied";
  case 0x67: return "ptt_collide";
  case 0x77: return "ptt_bonk";
  case 0xf0: return "call_options_invalid";
  case 0xf1: return "protection_invalid";
  case 0xf2: return "duplex_invalid";
  case 0xf3: return "mode_invalid";
  case 0xff: return "service_unsupported";
  default: return "";
  }
}

// TIA-102.AABC extended functions
char const *p25_ext_function_name(uint16_t function){
  switch(function){
  case 0x0000: return "radio_check";
  case 0x007d: return "radio_detach";
  case 0x007e: return "radio_uninhibit";
  case 0x007f: return "radio_inhibit";
  case 0x0080: return "radio_check_ack";
  case 0x00fd: return "radio_detach_ack";
  case 0x00fe: return "radio_uninhibit_ack";
  case 0x00ff: return "radio_inhibit_ack";
  case 0x0200: return "regroup_create";
  case 0x0201: return "regroup_cancel";
  case 0x0280: return "regroup_create_ack";
  case 0x0281: return "regroup_cancel_ack";
  default: return "";
  }
}

// TIA-102.BAAA service access points
char const *p25_sap_name(uint8_t sap){
  switch(sap){
  case 0: return "user_data";
  case 1: return "encrypted_user_data";
  case 2: return "circuit_data";
  case 3: return "circuit_data_control";
  case 4: return "packet_data";
  case 5: return "arp";
  case 6: return "sndcp_control";
  case 15: return "scan_preamble";
  case 29: return "encryption_support";
  case 31: return "extended_address";
  case 32: return "registration";
  case 33: return "channel_reassignment";
  case 34: return "system_configuration";
  case 35: return "loopback";
  case 36: return "statistics";
  case 37: return "out_of_service";
  case 38: return "paging";
  case 39: return "configuration";
  case 40: return "key_management";
  case 41: return "encrypted_key_management";
  case 48: return "location_service";
  case 61: return "trunking_control";
  case 63: return "encrypted_trunking_control";
  default: return "";
  }
}

// Formatting

struct out {
  char *p;
  size_t left;
  int n;
};

static void put(struct out *o, char const *fmt, ...){
  va_list ap;
  va_start(ap, fmt);
  int k = vsnprintf(o->p, o->left, fmt, ap);
  va_end(ap);
  if(k < 0)
    return;
  o->n += k;
  size_t const used = (size_t)k < o->left ? (size_t)k : (o->left ? o->left - 1 : 0);
  o->p += used;
  o->left -= used;
}

static void put_mhz(struct out *o, char const *key, uint64_t hz){
  put(o, " %s=%llu.%06llu", key, (unsigned long long)(hz / 1000000), (unsigned long long)(hz % 1000000));
}

// " ch=1-291 freq=851.637500" (and " slot=1" on TDMA); suffix names the second channel.
// An explicit uplink channel names the uplink frequency itself: no transmit offset is added.
static void put_chan(struct out *o, char const *suffix, struct p25_chan ch, struct p25_bandplan const *plan){
  put(o, " ch%s=%u-%u", suffix, ch.iden, ch.num);
  uint64_t dl;
  int slot;
  if(!p25_chan_freq(plan, ch, &dl, NULL, &slot))
    return;
  char key[16];
  snprintf(key, sizeof key, "freq%s", suffix);
  put_mhz(o, key, dl);
  struct p25_band const *b = &plan->band[ch.iden];
  if(b->tdma && b->slots > 1)
    put(o, " slot%s=%d", suffix, slot);
}

static void put_svc(struct out *o, uint8_t svc){
  put(o, " svc=0x%02x", svc);
  if(svc & P25_SVC_EMERGENCY)
    put(o, " emerg=1");
  if(svc & P25_SVC_ENCRYPTED)
    put(o, " enc=1");
  if(svc & P25_SVC_DUPLEX)
    put(o, " duplex=1");
  if(svc & P25_SVC_PACKET)
    put(o, " packet=1");
  put(o, " prio=%d", svc & P25_SVC_PRIORITY);
}

static void put_hex(struct out *o, char const *key, uint8_t const *p, int len){
  put(o, " %s=", key);
  for(int i = 0; i < len; i++)
    put(o, "%02x", p[i]);
}

static void put_reason(struct out *o, uint8_t reason){
  put(o, " reason=0x%02x", reason);
  char const *name = p25_deny_reason_name(reason);
  if(*name)
    put(o, " reason_name=%s", name);
}

static void put_iden(struct out *o, struct p25_msg const *m){
  put(o, " iden=%u", m->iden);
  if(m->kind == P25_IDEN_UP_TDMA)
    put(o, " type=%u tdma=%d slots=%u", m->iden_type, m->iden_tdma, m->iden_slots);
  else if(m->kind == P25_IDEN_UP_VU)
    put(o, " bw_code=%u", m->iden_type);
  put_mhz(o, "base", m->base_hz);
  put(o, " spacing=%u bw=%u tx_offset=%lld", m->spacing_hz, m->bw_hz, (long long)m->tx_offset_hz);
}

static void put_site(struct out *o, struct p25_msg const *m){
  if(m->family != P25_FAMILY_MBT)
    put(o, " lra=0x%02x", m->lra);
  put(o, " sys=0x%03x rfss=%u site=%u", m->sysid, m->rfss, m->site);
}

static void put_flags(struct out *o, struct p25_msg const *m){
  if(m->kind == P25_ADJ_STS_BCST && m->family == P25_FAMILY_TSBK)
    put(o, " conv=%d fail=%d valid=%d", (m->site_flags >> 3) & 1, (m->site_flags >> 2) & 1,
        (m->site_flags >> 1) & 1);
  if(m->family == P25_FAMILY_TSBK)
    put(o, " net=%d", m->site_flags & 1);
}

int p25_format(char *buf, size_t size, struct p25_msg const *m, struct p25_bandplan const *plan){
  struct out o = { .p = buf, .left = size, .n = 0 };
  if(size > 0)
    buf[0] = '\0';
  put(&o, "%s %s", p25_family_name(m->family), p25_kind_name(m->kind));
  bool const lc = m->family == P25_FAMILY_LC || m->family == P25_FAMILY_TDULC;
  if(m->family == P25_FAMILY_TSBK || m->family == P25_FAMILY_MBT || lc){
    if(m->kind == P25_UNKNOWN || m->kind == P25_LC_OTHER || m->mfid != P25_MFID_STD)
      put(&o, " mfid=0x%02x opcode=0x%02x", m->mfid, m->opcode);
    if(m->protect)
      put(&o, " protected=1");
  }
  if(lc && m->protect){
    put_hex(&o, "lc", m->raw, 9);
    return o.n;
  }

  switch(m->kind){
  case P25_UNKNOWN:
  case P25_LC_OTHER:
  case P25_GRP_DATA_CH_ANN:
  case P25_GRP_DATA_CH_ANN_EXP:
  case P25_SN_DATA_PAGE_REQ:
  case P25_AUTH_DMAND:
  case P25_AUTH_FNE_RESP:
  case P25_ROAM_ADDR_UPDT:
  case P25_ADJ_STS_BCST_UNCOORD:
  case P25_LC_SRC_ID_EXT:
  case P25_LC_MOT_FAILSOFT:
  case P25_LC_HARRIS_GPS1:
  case P25_LC_HARRIS_GPS2:
    if(m->family == P25_FAMILY_MBT){
      put(&o, " sap=%u llid=%u blocks=%u", m->sap, m->llid, m->blocks);
      put_hex(&o, "hdr", m->raw, 12);
    } else
      put_hex(&o, "raw", m->raw, m->raw_len);
    break;
  case P25_GRP_V_CH_GRANT:
  case P25_GRP_DATA_CH_GRANT:
  case P25_MOT_GRG_CH_GRANT:
    put(&o, " tg=%u", m->group);
    if(m->kind != P25_GRP_DATA_CH_GRANT || m->family == P25_FAMILY_TSBK)
      put(&o, " src=%u", m->src);
    if(m->kind != P25_GRP_DATA_CH_GRANT || m->family == P25_FAMILY_TSBK)
      put_svc(&o, m->svc);
    put_chan(&o, "", m->ch, plan);
    break;
  case P25_GRP_V_CH_GRANT_UPDT:
  case P25_MOT_GRG_CH_UPDT:
    put(&o, " tg=%u", m->group);
    put_chan(&o, "", m->ch, plan);
    if(m->has_b){
      put(&o, " tg_b=%u", m->group_b);
      put_chan(&o, "_b", m->ch_b, plan);
    }
    break;
  case P25_GRP_V_CH_GRANT_UPDT_EXP:
    put(&o, " tg=%u", m->group);
    put_svc(&o, m->svc);
    put_chan(&o, "", m->ch, plan);
    put_chan(&o, "_ul", m->ch_b, plan);
    break;
  case P25_UU_V_CH_GRANT:
  case P25_UU_V_CH_GRANT_UPDT:
  case P25_IND_DATA_CH_GRANT:
    if(m->family != P25_FAMILY_MBT || m->kind != P25_IND_DATA_CH_GRANT)
      put(&o, " src=%u", m->src);
    put(&o, " tgt=%u", m->tgt);
    if(m->family == P25_FAMILY_MBT && m->kind != P25_IND_DATA_CH_GRANT)
      put_svc(&o, m->svc);
    put_chan(&o, "", m->ch, plan);
    break;
  case P25_UU_ANS_REQ:
  case P25_STS_Q:
  case P25_CALL_ALRT:
  case P25_GRP_AFF_Q:
  case P25_RAD_MON_CMD:
  case P25_LC_HARRIS_RETURN_CC:
    put(&o, " src=%u tgt=%u", m->src, m->tgt);
    if(m->kind == P25_UU_ANS_REQ && !lc)
      put_svc(&o, m->svc);
    if(m->kind == P25_RAD_MON_CMD)
      put(&o, " tx_mult=%u", m->tx_mult);
    break;
  case P25_TELE_INT_CH_GRANT:
  case P25_TELE_INT_CH_GRANT_UPDT:
    put(&o, " unit=%u timer_ms=%u", m->tgt, m->timer * 100);
    put_svc(&o, m->svc);
    put_chan(&o, "", m->ch, plan);
    break;
  case P25_TELE_INT_ANS_REQ:
    if(!lc)
      put(&o, " unit=%u timer=%u", m->tgt, m->timer);
    else
      put_hex(&o, "lc", m->raw, 9);
    break;
  case P25_SN_DATA_CH_GRANT:
    put(&o, " tgt=%u", m->tgt);
    put_svc(&o, m->svc);
    put_chan(&o, "", m->ch, plan);
    if(m->has_b)
      put_chan(&o, "_ul", m->ch_b, plan);
    break;
  case P25_SN_DATA_CH_ANN_EXP:
    put_svc(&o, m->svc);
    put(&o, " autonomous=%d requested=%d dac=0x%04x", m->autonomous, m->requested, m->data_access);
    put_chan(&o, "", m->ch, plan);
    put_chan(&o, "_ul", m->ch_b, plan);
    break;
  case P25_STS_UPDT:
    put(&o, " src=%u tgt=%u unit_status=%u user_status=%u", m->src, m->tgt, m->unit_status, m->user_status);
    break;
  case P25_MSG_UPDT:
    put(&o, " src=%u tgt=%u msg=0x%04x", m->src, m->tgt, m->message);
    break;
  case P25_ACK_RSP_FNE:
  case P25_MOT_ACK_RSP:
    put(&o, " tgt=%u src=%u svc_type=0x%02x aiv=%d ex=%d", m->tgt, m->src, m->svc_type, m->aiv, m->ex);
    break;
  case P25_QUE_RSP:
  case P25_DENY_RSP:
  case P25_MOT_QUE_RSP:
  case P25_MOT_DENY_RSP:
    put(&o, " tgt=%u svc_type=0x%02x", m->tgt, m->svc_type);
    put_reason(&o, m->reason);
    put(&o, " aiv=%d info=%u", m->aiv, m->info);
    break;
  case P25_EXT_FNCT_CMD:
    if(lc){
      put_hex(&o, "lc", m->raw, 9);
      break;
    }
    put(&o, " tgt=%u function=0x%04x", m->tgt, m->function);
    if(*p25_ext_function_name(m->function))
      put(&o, " function_name=%s", p25_ext_function_name(m->function));
    put(&o, " args=0x%06x", m->args);
    break;
  case P25_GRP_AFF_RSP:
    put(&o, " tgt=%u tg=%u ann_tg=%u response=%u lg=%d", m->tgt, m->group, m->group_b, m->response,
        m->local_global);
    break;
  case P25_LOC_REG_RSP:
    put(&o, " tgt=%u tg=%u rfss=%u site=%u response=%u", m->tgt, m->group, m->rfss, m->site, m->response);
    break;
  case P25_U_REG_RSP:
    put(&o, " src=%u src_id=%u sys=0x%03x response=%u", m->src, m->info, m->sysid, m->response);
    break;
  case P25_U_REG_CMD:
  case P25_AUTH_CMD:
  case P25_ROAM_ADDR_CMD:
    if(lc)
      put_hex(&o, "lc", m->raw, 9);
    else
      put(&o, " tgt=%u", m->tgt);
    break;
  case P25_U_DE_REG_ACK:
    put(&o, " src=%u wacn=0x%05x sys=0x%03x", m->src, m->wacn, m->sysid);
    break;
  case P25_SYNC_BCST:
    put(&o, " date=%04u-%02u-%02u time=%02u:%02u microslots=%u", m->year, m->month, m->day, m->hour,
        m->minute, m->microslots);
    if(!m->lto_valid_bit)
      put(&o, " utc_offset=%c%u:%02u", m->lto_negative ? '-' : '+', m->lto_hours, m->lto_half ? 30 : 0);
    put(&o, " unlocked=%d uslot_unlocked=%d leap=%u", m->time_unlocked, m->uslot_unlocked, m->leap);
    break;
  case P25_TIME_DATE_ANN:
    put(&o, " date=%04u-%02u-%02u time=%02u:%02u", m->year, m->month, m->day, m->hour, m->minute);
    break;
  case P25_IDEN_UP:
  case P25_IDEN_UP_TDMA:
  case P25_IDEN_UP_VU:
    put_iden(&o, m);
    break;
  case P25_SYS_SRV_BCST:
    put(&o, " avail=0x%06x supp=0x%06x", m->avail_svc, m->supp_svc);
    if(!lc)
      put(&o, " req_prio=%u", m->req_priority);
    break;
  case P25_SCCB:
    put(&o, " rfss=%u site=%u", m->rfss, m->site);
    put_chan(&o, "", m->ch, plan);
    put(&o, " class=0x%02x", m->svc_class);
    if(m->has_b){
      put_chan(&o, "_b", m->ch_b, plan);
      put(&o, " class_b=0x%02x", m->svc_class_b);
    }
    break;
  case P25_SCCB_EXP:
    put(&o, " rfss=%u site=%u", m->rfss, m->site);
    put_chan(&o, "", m->ch, plan);
    if(m->has_b)
      put_chan(&o, "_ul", m->ch_b, plan);
    put(&o, " class=0x%02x", m->svc_class);
    break;
  case P25_RFSS_STS_BCST:
  case P25_RFSS_STS_BCST_EXP:
  case P25_ADJ_STS_BCST:
  case P25_ADJ_STS_BCST_EXP:
    put_site(&o, m);
    put_chan(&o, "", m->ch, plan);
    if(m->has_b)
      put_chan(&o, "_ul", m->ch_b, plan);
    if(m->family != P25_FAMILY_MBT)
      put(&o, " class=0x%02x", m->svc_class);
    if(m->kind == P25_RFSS_STS_BCST || m->kind == P25_ADJ_STS_BCST)
      put_flags(&o, m);
    break;
  case P25_NET_STS_BCST:
  case P25_NET_STS_BCST_EXP:
    if(m->family == P25_FAMILY_TSBK)
      put(&o, " lra=0x%02x", m->lra);
    put(&o, " wacn=0x%05x sys=0x%03x", m->wacn, m->sysid);
    put_chan(&o, "", m->ch, plan);
    if(m->has_b)
      put_chan(&o, "_ul", m->ch_b, plan);
    if(m->family != P25_FAMILY_MBT && m->kind == P25_NET_STS_BCST)
      put(&o, " class=0x%02x", m->svc_class);
    break;
  case P25_MOT_GRG_ADD:
  case P25_MOT_GRG_DEL:
    put(&o, " sg=%u tgs=", m->group);
    for(int i = 0; i < m->nmembers; i++)
      put(&o, i ? ",%u" : "%u", m->members[i]);
    break;
  case P25_MOT_EXT_FNCT_CMD:
    put(&o, " tgt=%u class=0x%02x operand=0x%02x args=0x%06x", m->tgt, m->ext_class, m->ext_operand, m->args);
    break;
  case P25_MOT_TRAFFIC_CH_ID:
    put(&o, " tch1=%u tch2=%u", m->tch1, m->tch2);
    break;
  case P25_MOT_SYS_LOADING:
    put(&o, " load=%u capacity=%u", m->load, m->capacity);
    break;
  case P25_MOT_EMERG_ALRM:
    put(&o, " tg=%u src=%u", m->group, m->src);
    break;
  case P25_MOT_BSI:
    put(&o, " cwid=\"%s\"", m->cwid);
    put_chan(&o, "", m->ch, plan);
    break;
  case P25_MOT_CC_SHUTDOWN:
    put(&o, " minutes=%u", m->shutdown_min);
    break;
  case P25_MOT_TDMA_DATA_CH:
    put_chan(&o, "", m->ch, plan);
    put_chan(&o, "_ul", m->ch_b, plan);
    break;
  case P25_HARRIS_GRG_EXENC_CMD:
    put(&o, " sg=%u %s %s seq=%u key=0x%04x opts=0x%02x", m->group,
        (m->regroup_opts & 0x20) ? "activate=1" : "activate=0",
        (m->regroup_opts & 0x80) ? "simulselect=1" : "simulselect=0", m->regroup_opts & 0x1f, m->key_id,
        m->regroup_opts);
    if(m->regroup_opts & 0x40)
      put(&o, " tg=%u alg=0x%02x", m->group_b, m->alg_id);
    else
      put(&o, " radio=%u", m->tgt);
    break;
  case P25_LC_GRP_V_CH_USR:
  case P25_LC_MOT_GRG_V_CH_USR:
    put(&o, " tg=%u src=%u", m->group, m->src);
    put_svc(&o, m->svc);
    break;
  case P25_LC_UU_V_CH_USR:
    put(&o, " tgt=%u src=%u", m->tgt, m->src);
    put_svc(&o, m->svc);
    break;
  case P25_LC_TEL_INT_V_CH_USR:
    put(&o, " unit=%u timer_ms=%u", m->tgt, m->call_timer_ms);
    put_svc(&o, m->svc);
    break;
  case P25_LC_UU_V_CH_USR_EXT:
    put(&o, " tgt=%u", m->tgt);
    put_svc(&o, m->svc);
    break;
  case P25_LC_CALL_TRM:
  case P25_LC_MOT_TALK_COMPLETE:
    put(&o, " addr=%u", m->address);
    // the system controller's address: the network tore the channel down
    if(m->kind == P25_LC_CALL_TRM && (m->address == 0xfffffd || m->address == 0xffffff || m->address == 0))
      put(&o, " network=1");
    break;
  case P25_LC_MOT_GRG_V_CH_UPDT:
    put(&o, " tg=%u", m->group);
    put_svc(&o, m->svc);
    put_chan(&o, "", m->ch, plan);
    break;
  case P25_LC_MOT_UNIT_GPS:
    put(&o, " lat=%.6f lon=%.6f", m->latitude, m->longitude);
    break;
  case P25_LC_MOT_ALIAS_HDR:
    put(&o, " tg=%u blocks=%u seq=%u format=%u", m->group, m->block_count, m->sequence, m->alias_format);
    break;
  case P25_LC_MOT_ALIAS_DATA:
    put(&o, " block=%u seq=%u", m->block, m->sequence);
    put_hex(&o, "data", m->raw + 3, 6);
    break;
  case P25_LC_HARRIS_ALIAS:
    put(&o, " block=%u text=\"%s\"", m->block, m->text);
    break;
  case P25_PDU_RESPONSE:
    put(&o, " llid=%u response=0x%02x", m->llid, m->pdu_response);
    if(m->has_src_llid)
      put(&o, " src=%u", m->src_llid);
    put(&o, " dir=%s", m->outbound ? "out" : "in");
    break;
  case P25_PDU_SNDCP:
  case P25_PDU_SNDCP_DATA:
  case P25_PDU_DATA:
    put(&o, " llid=%u sap=%u", m->llid, m->sap);
    if(*p25_sap_name(m->sap))
      put(&o, " sap_name=%s", p25_sap_name(m->sap));
    put(&o, " confirmed=%d dir=%s octets=%d", m->confirmed, m->outbound ? "out" : "in", m->user_octets);
    if(m->kind == P25_PDU_SNDCP && m->user_octets > 0){
      put(&o, " sndcp_type=%u nsapi=%u", m->sndcp_type, m->sndcp_nsapi);
      if(m->sndcp_has_arg)
        put(&o, " arg=%u", m->sndcp_arg);
      if(m->sndcp_has_ip)
        put(&o, " ip=%u.%u.%u.%u", m->sndcp_ip[0], m->sndcp_ip[1], m->sndcp_ip[2], m->sndcp_ip[3]);
    }
    break;
  case P25_PDU_OTHER:
    put(&o, " format=0x%02x sap=%u mfid=0x%02x llid=%u blocks=%u confirmed=%d dir=%s", m->format, m->sap,
        m->mfid, m->llid, m->blocks, m->confirmed, m->outbound ? "out" : "in");
    put_hex(&o, "hdr", m->raw, 12);
    break;
  case P25_HDU:
    put(&o, " mfid=0x%02x", m->mfid);
    if(m->mfid == P25_MFID_STD){
      put(&o, " tg=%u algid=0x%02x kid=0x%04x", m->group, m->algid, m->kid);
      put_hex(&o, "mi", m->mi, 9);
    }
    break;
  case P25_ENC_SYNC:
    put(&o, " algid=0x%02x kid=0x%04x", m->algid, m->kid);
    put_hex(&o, "mi", m->mi, 9);
    if(m->zero_es)
      put(&o, " (sent as all zeros: taken as unencrypted)");
    break;
  case P25_KIND_COUNT:
    break;
  }
  return o.n;
}
