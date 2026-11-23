// P25 Phase 1 message decoding: TSBKs (TIA-102.AABC), link control (TIA-102.AABF),
// alternate multi-block trunking and packet data headers (TIA-102.BAAA), HDU and LDU2
// encryption sync, into the shared message of p25msg.h
// Copyright 2026, Matthew Kaufman, KA6SQG

#include <string.h>
#include "p25msg.h"
#include "p25p1msg.h"

// Theory of operation
//
// What arrives. Each decoder takes a payload p25p1unit.c has already error corrected and, where
// the payload has one, checked by its CRC; nothing here checks anything. Each zeroes the
// message, sets the family, takes the opcode and MFID, keeps the first octets raw for the log,
// chooses the kind from the opcode table of that MFID, and reads that kind's fields by bit
// position. Positions count from the most significant bit of octet 0, as the standards number
// them, and field() reads any run of up to 32 bits.
//
// Trunking signalling blocks. A TSBK is 12 octets: last block flag, protected flag and 6-bit
// opcode, the MFID, eight octets of arguments and the CRC-16. A trunking data unit carries one
// to three; each is decoded on its own, and last_block is passed on. The protected flag is
// reported, but the fields are read as for a clear block. Many opcodes share a layout: two
// grant updates in one block (two_grants()), the response layout (response()), and the three
// identifier update layouts.
//
// Link control. The 72-bit word of an LDU1 or a TDULC: protected flag, standard format flag
// and opcode, then the MFID in octet 1 unless the standard format flag says the MFID is
// implicit. A protected word is encrypted past those, so only they are read.
// Where link control carries a message a TSBK also carries (grant updates, status and site
// broadcasts) it gets the TSBK's kind, in link control's own layout, so the call tracking acts
// on it the same way; what only link control carries (the voice channel user, which says who
// holds the channel now, and call termination) has its own P25_LC_ kinds.
//
// Packet data. Every packet starts with a 12-octet header whose format field says what follows.
// An alternate multi-block trunking control (AMBTC, format 0x17) on the trunking SAP is a
// trunking message too long for one TSBK, its fields spread over the header and the first data
// block, decoded here into the same kinds as the TSBK forms. Confirmed and unconfirmed data
// on the trunking SAPs (UMBTC) is logged from the header only. Other data is counted (user
// octets, with pad and CRC-32 removed) and, for SNDCP context control, its first octets
// decoded. A response packet gives its class, type and status as one octet.
//
// Voice headers. The HDU and an LDU2's encryption sync carry the message indicator, the
// algorithm and the key ID. Only the all-zero encryption sync is excused (p25_decode_es());
// an ALGID of 0x00 anywhere else is an algorithm, and so encrypted.

// Bits [off, off+len) of an MSB-first octet string, len <= 32
static uint32_t field(uint8_t const *p, int off, int len){
  uint32_t v = 0;
  for(int i = off; i < off + len; i++)
    v = (v << 1) | ((p[i >> 3] >> (7 - (i & 7))) & 1);
  return v;
}
static bool bit(uint8_t const *p, int off){
  return (p[off >> 3] >> (7 - (off & 7))) & 1;
}
static struct p25_chan chan(uint8_t const *p, int off){
  struct p25_chan c = { .iden = field(p, off, 4), .num = field(p, off + 4, 12) };
  return c;
}
static void keep_raw(struct p25_msg *msg, uint8_t const *p, int len){
  if(len > (int)sizeof msg->raw)
    len = sizeof msg->raw;
  memcpy(msg->raw, p, len);
  msg->raw_len = len;
}

// Identifier updates (TIA-102.AABC IDEN_UP, IDEN_UP_TDMA, IDEN_UP_VU)
static void iden_fdma(struct p25_msg *msg, uint8_t const *p){
  msg->iden = field(p, 16, 4);
  msg->bw_hz = field(p, 20, 9) * 125;
  int64_t off = (int64_t)field(p, 30, 8) * 250000;  // sign bit 29: 1 positive
  msg->tx_offset_hz = bit(p, 29) ? off : -off;
  msg->spacing_hz = field(p, 38, 10) * 125;
  msg->base_hz = (uint64_t)field(p, 48, 32) * 5;
  msg->iden_slots = 1;
}
// TDMA and VHF/UHF share a layout: 4-bit type, 13-bit offset in channel spacings
static void iden_13bit(struct p25_msg *msg, uint8_t const *p){
  msg->iden = field(p, 16, 4);
  msg->iden_type = field(p, 20, 4);
  msg->spacing_hz = field(p, 38, 10) * 125;
  int64_t off = (int64_t)field(p, 25, 13) * msg->spacing_hz;
  msg->tx_offset_hz = bit(p, 24) ? off : -off;
  msg->base_hz = (uint64_t)field(p, 48, 32) * 5;
}
// TDMA channel types: slots per channel and bandwidth
// Types 0-2 are FDMA, 3-5 TDMA (iden_tdma() below); a type not listed reads as one slot with
// no bandwidth, and is not TDMA
static const struct { uint8_t slots; uint16_t bw; } Tdma_type[16] = {
  [0] = { 1, 12500 }, [1] = { 1, 12500 }, [2] = { 1, 6250 }, [3] = { 2, 12500 },
  [4] = { 4, 25000 }, [5] = { 2, 12500 },
};
static void iden_tdma(struct p25_msg *msg, uint8_t const *p){
  iden_13bit(msg, p);
  int t = msg->iden_type;
  msg->iden_slots = Tdma_type[t].slots ? Tdma_type[t].slots : 1;
  msg->bw_hz = Tdma_type[t].bw;
  msg->iden_tdma = (t == 3 || t == 4 || t == 5);
}
static void iden_vu(struct p25_msg *msg, uint8_t const *p){
  iden_13bit(msg, p);
  msg->bw_hz = msg->iden_type == 4 ? 6250 : msg->iden_type == 5 ? 12500 : 0;
  msg->iden_slots = 1;
}

// Two talkgroup/channel pairs; the second only if it names another group
static void two_grants(struct p25_msg *msg, uint8_t const *p){
  msg->ch = chan(p, 16);
  msg->group = field(p, 32, 16);
  msg->ch_b = chan(p, 48);
  msg->group_b = field(p, 64, 16);
  msg->has_b = msg->group_b != 0 && msg->group_b != msg->group;
}
// Response layout: AIV, EX, service type, reason, additional info, target
static void response(struct p25_msg *msg, uint8_t const *p){
  msg->aiv = bit(p, 16);
  msg->ex = bit(p, 17);
  msg->svc_type = field(p, 18, 6);
  msg->reason = field(p, 24, 8);
  msg->info = field(p, 32, 24);
  msg->tgt = field(p, 56, 24);
}

// The standard opcodes (both standard MFIDs). Bit positions are the TSBK's, opcode at 2 and
// arguments from 16. Opcodes that are only named (data announcements, the data paging request,
// authentication, roaming address update, the uncoordinated adjacent status) are logged raw
static void tsbk_standard(struct p25_msg *msg, uint8_t const *p){
  switch(msg->opcode){
  case 0x00:
    msg->kind = P25_GRP_V_CH_GRANT;
    msg->svc = field(p, 16, 8);
    msg->ch = chan(p, 24);
    msg->group = field(p, 40, 16);
    msg->src = field(p, 56, 24);
    break;
  case 0x02:
    msg->kind = P25_GRP_V_CH_GRANT_UPDT;
    two_grants(msg, p);
    break;
  case 0x03:
    msg->kind = P25_GRP_V_CH_GRANT_UPDT_EXP;
    msg->svc = field(p, 16, 8);
    msg->ch = chan(p, 32);
    msg->ch_b = chan(p, 48);
    msg->has_b = true;
    msg->group = field(p, 64, 16);
    break;
  case 0x04:
  case 0x06: // the update has the grant's layout
    msg->kind = msg->opcode == 0x04 ? P25_UU_V_CH_GRANT : P25_UU_V_CH_GRANT_UPDT;
    msg->ch = chan(p, 16);
    msg->tgt = field(p, 32, 24);
    msg->src = field(p, 56, 24);
    break;
  case 0x05:
    msg->kind = P25_UU_ANS_REQ;
    msg->svc = field(p, 16, 8);
    msg->tgt = field(p, 32, 24);
    msg->src = field(p, 56, 24);
    break;
  case 0x08:
  case 0x09:
    msg->kind = msg->opcode == 0x08 ? P25_TELE_INT_CH_GRANT : P25_TELE_INT_CH_GRANT_UPDT;
    msg->svc = field(p, 16, 8);
    msg->ch = chan(p, 24);
    msg->timer = field(p, 40, 16);
    msg->tgt = field(p, 56, 24);
    break;
  case 0x0a:
    msg->kind = P25_TELE_INT_ANS_REQ;
    msg->tgt = field(p, 16, 24);
    msg->timer = field(p, 40, 16);
    break;
  case 0x10:
    msg->kind = P25_IND_DATA_CH_GRANT;
    msg->ch = chan(p, 16);
    msg->tgt = field(p, 32, 24);
    msg->src = field(p, 56, 24);
    break;
  case 0x11:
    msg->kind = P25_GRP_DATA_CH_GRANT;
    msg->svc = field(p, 16, 8);
    msg->ch = chan(p, 24);
    msg->group = field(p, 40, 16);
    msg->src = field(p, 56, 24);
    break;
  case 0x12:
    msg->kind = P25_GRP_DATA_CH_ANN;
    break;
  case 0x13:
    msg->kind = P25_GRP_DATA_CH_ANN_EXP;
    break;
  case 0x14:
    msg->kind = P25_SN_DATA_CH_GRANT;
    msg->svc = field(p, 16, 8);
    msg->ch = chan(p, 24);
    msg->ch_b = chan(p, 40);
    msg->has_b = msg->ch_b.num != 4095; // channel 4095: no uplink
    msg->tgt = field(p, 56, 24);
    break;
  case 0x15:
    msg->kind = P25_SN_DATA_PAGE_REQ;
    break;
  case 0x16:
    msg->kind = P25_SN_DATA_CH_ANN_EXP;
    msg->svc = field(p, 16, 8);
    msg->autonomous = bit(p, 24);
    msg->requested = bit(p, 25);
    msg->ch = chan(p, 32);
    msg->ch_b = chan(p, 48);
    msg->has_b = true;
    msg->data_access = field(p, 64, 16);
    break;
  case 0x18:
    msg->kind = P25_STS_UPDT;
    msg->unit_status = field(p, 16, 8);
    msg->user_status = field(p, 24, 8);
    msg->tgt = field(p, 32, 24);
    msg->src = field(p, 56, 24);
    break;
  case 0x1a:
    msg->kind = P25_STS_Q;
    msg->tgt = field(p, 32, 24);
    msg->src = field(p, 56, 24);
    break;
  case 0x1c:
    msg->kind = P25_MSG_UPDT;
    msg->message = field(p, 16, 16);
    msg->tgt = field(p, 32, 24);
    msg->src = field(p, 56, 24);
    break;
  case 0x1d: // source before target, unlike most
    msg->kind = P25_RAD_MON_CMD;
    msg->tx_mult = field(p, 30, 2);
    msg->src = field(p, 32, 24);
    msg->tgt = field(p, 56, 24);
    break;
  case 0x1f:
    msg->kind = P25_CALL_ALRT;
    msg->tgt = field(p, 32, 24);
    msg->src = field(p, 56, 24);
    break;
  case 0x20: // additional information (32-55) is the source address
    msg->kind = P25_ACK_RSP_FNE;
    msg->aiv = bit(p, 16);
    msg->ex = bit(p, 17);
    msg->svc_type = field(p, 18, 6);
    msg->src = field(p, 32, 24);
    msg->tgt = field(p, 56, 24);
    break;
  case 0x21:
    msg->kind = P25_QUE_RSP;
    response(msg, p);
    break;
  case 0x24:
    msg->kind = P25_EXT_FNCT_CMD;
    msg->function = field(p, 16, 16);
    msg->args = field(p, 32, 24);
    msg->tgt = field(p, 56, 24);
    break;
  case 0x27:
    msg->kind = P25_DENY_RSP;
    response(msg, p);
    break;
  case 0x28: // LG, response, announcement group, group, target
    msg->kind = P25_GRP_AFF_RSP;
    msg->local_global = bit(p, 16);
    msg->response = field(p, 22, 2);
    msg->group_b = field(p, 24, 16);
    msg->group = field(p, 40, 16);
    msg->tgt = field(p, 56, 24);
    break;
  case 0x29:
    msg->kind = P25_SCCB_EXP;
    msg->rfss = field(p, 16, 8);
    msg->site = field(p, 24, 8);
    msg->ch = chan(p, 32);
    msg->ch_b = chan(p, 56);
    msg->has_b = true;
    msg->svc_class = field(p, 72, 8);
    break;
  case 0x2a:
    msg->kind = P25_GRP_AFF_Q;
    msg->tgt = field(p, 32, 24);
    msg->src = field(p, 56, 24);
    break;
  case 0x2b:
    msg->kind = P25_LOC_REG_RSP;
    msg->response = field(p, 22, 2);
    msg->group = field(p, 24, 16);
    msg->rfss = field(p, 40, 8);
    msg->site = field(p, 48, 8);
    msg->tgt = field(p, 56, 24);
    break;
  case 0x2c: // source ID in info, source address in src
    msg->kind = P25_U_REG_RSP;
    msg->response = field(p, 18, 2);
    msg->sysid = field(p, 20, 12);
    msg->info = field(p, 32, 24);
    msg->src = field(p, 56, 24);
    break;
  case 0x2d:
    msg->kind = P25_U_REG_CMD;
    msg->tgt = field(p, 16, 24);
    break;
  case 0x2e:
    msg->kind = P25_AUTH_CMD;
    msg->tgt = field(p, 16, 24);
    break;
  case 0x2f: // WACN, system, source
    msg->kind = P25_U_DE_REG_ACK;
    msg->wacn = field(p, 24, 20);
    msg->sysid = field(p, 44, 12);
    msg->src = field(p, 56, 24);
    break;
  case 0x30: // TIA-102.AABB time synchronisation
    msg->kind = P25_SYNC_BCST;
    msg->time_unlocked = bit(p, 29);
    msg->uslot_unlocked = bit(p, 30);
    msg->leap = field(p, 31, 2);
    msg->lto_valid_bit = bit(p, 33);
    msg->lto_negative = bit(p, 34);
    msg->lto_hours = field(p, 35, 4);
    msg->lto_half = bit(p, 39);
    msg->year = 2000 + field(p, 40, 7);
    msg->month = field(p, 47, 4);
    msg->day = field(p, 51, 5);
    msg->hour = field(p, 56, 5);
    msg->minute = field(p, 61, 6);
    msg->microslots = field(p, 67, 13);
    break;
  case 0x31:
    msg->kind = P25_AUTH_DMAND;
    break;
  case 0x32:
    msg->kind = P25_AUTH_FNE_RESP;
    break;
  case 0x33:
    msg->kind = P25_IDEN_UP_TDMA;
    iden_tdma(msg, p);
    break;
  case 0x34:
    msg->kind = P25_IDEN_UP_VU;
    iden_vu(msg, p);
    break;
  case 0x35:
    msg->kind = P25_TIME_DATE_ANN;
    msg->year = 2000 + field(p, 16, 7);
    msg->month = field(p, 23, 4);
    msg->day = field(p, 27, 5);
    msg->hour = field(p, 32, 5);
    msg->minute = field(p, 37, 6);
    break;
  case 0x36:
    msg->kind = P25_ROAM_ADDR_CMD;
    msg->tgt = field(p, 16, 24);
    break;
  case 0x37:
    msg->kind = P25_ROAM_ADDR_UPDT;
    break;
  case 0x38:
    msg->kind = P25_SYS_SRV_BCST;
    msg->avail_svc = field(p, 24, 24);
    msg->supp_svc = field(p, 48, 24);
    msg->req_priority = field(p, 72, 8);
    break;
  case 0x39:
    msg->kind = P25_SCCB;
    msg->rfss = field(p, 16, 8);
    msg->site = field(p, 24, 8);
    msg->ch = chan(p, 32);
    msg->svc_class = field(p, 48, 8);
    msg->ch_b = chan(p, 56);
    msg->svc_class_b = field(p, 72, 8);
    msg->has_b = msg->ch_b.num != 0 && msg->ch_b.num != msg->ch.num;
    break;
  case 0x3a:
    msg->kind = P25_RFSS_STS_BCST;
    msg->lra = field(p, 16, 8);
    msg->site_flags = bit(p, 27);
    msg->sysid = field(p, 28, 12);
    msg->rfss = field(p, 40, 8);
    msg->site = field(p, 48, 8);
    msg->ch = chan(p, 56);
    msg->svc_class = field(p, 72, 8);
    break;
  case 0x3b:
    msg->kind = P25_NET_STS_BCST;
    msg->lra = field(p, 16, 8);
    msg->wacn = field(p, 24, 20);
    msg->sysid = field(p, 44, 12);
    msg->ch = chan(p, 56);
    msg->svc_class = field(p, 72, 8);
    break;
  case 0x3c:
    msg->kind = P25_ADJ_STS_BCST;
    msg->lra = field(p, 16, 8);
    msg->site_flags = field(p, 24, 4);
    msg->sysid = field(p, 28, 12);
    msg->rfss = field(p, 40, 8);
    msg->site = field(p, 48, 8);
    msg->ch = chan(p, 56);
    msg->svc_class = field(p, 72, 8);
    break;
  case 0x3d:
    msg->kind = P25_IDEN_UP;
    iden_fdma(msg, p);
    break;
  case 0x3e:
    msg->kind = P25_ADJ_STS_BCST_UNCOORD;
    break;
  default: // reserved
    break;
  }
}

// Motorola base station ID: six-bit characters, A-Z 1-26, 0-9 27-36, '-' 37, '/' 38, 0 ends
static void cwid(char *out, uint8_t const *p){
  int n = 0;
  for(int i = 0; i < 8; i++){
    int c = field(p, 16 + 6 * i, 6);
    if(c == 0)
      break;
    out[n++] = c <= 26 ? 'A' + c - 1 : c <= 36 ? '0' + c - 27 : c == 37 ? '-' : c == 38 ? '/' : '?';
  }
  out[n] = '\0';
}

// Motorola's opcodes (MFID 0x90): regroups (patches and supergroups) and their grants,
// responses, alarms and site information. The layouts follow the standard ones where
// Motorola's message has a standard counterpart (the grant, the grant update, the responses)
static void tsbk_motorola(struct p25_msg *msg, uint8_t const *p){
  switch(msg->opcode){
  case 0x00:
  case 0x01: // supergroup and up to three members, each new, nonzero and not the supergroup
    msg->kind = msg->opcode == 0x00 ? P25_MOT_GRG_ADD : P25_MOT_GRG_DEL;
    msg->group = field(p, 16, 16);
    for(int i = 0; i < 3; i++){
      uint16_t g = field(p, 32 + 16 * i, 16);
      if(g == 0 || g == msg->group)
        continue;
      bool dup = false;
      for(int j = 0; j < msg->nmembers; j++)
        dup |= msg->members[j] == g;
      if(!dup)
        msg->members[msg->nmembers++] = g;
    }
    break;
  case 0x02:
    msg->kind = P25_MOT_GRG_CH_GRANT;
    msg->svc = field(p, 16, 8);
    msg->ch = chan(p, 24);
    msg->group = field(p, 40, 16);
    msg->src = field(p, 56, 24);
    break;
  case 0x03:
    msg->kind = P25_MOT_GRG_CH_UPDT;
    two_grants(msg, p);
    break;
  case 0x04:
    msg->kind = P25_MOT_EXT_FNCT_CMD;
    msg->ext_class = field(p, 16, 8);
    msg->ext_operand = field(p, 24, 8);
    msg->args = field(p, 32, 24);
    msg->tgt = field(p, 56, 24);
    break;
  case 0x05:
    msg->kind = P25_MOT_TRAFFIC_CH_ID;
    msg->tch1 = field(p, 16, 12);
    msg->tch2 = field(p, 28, 12);
    break;
  case 0x06:
  case 0x07: // the standard response layout
    msg->kind = msg->opcode == 0x06 ? P25_MOT_QUE_RSP : P25_MOT_DENY_RSP;
    response(msg, p);
    break;
  case 0x08:
    msg->kind = P25_MOT_ACK_RSP;
    msg->aiv = bit(p, 16);
    msg->ex = bit(p, 17);
    msg->svc_type = field(p, 18, 6);
    msg->src = field(p, 32, 24);
    msg->tgt = field(p, 56, 24);
    break;
  case 0x09:
    msg->kind = P25_MOT_SYS_LOADING;
    msg->load = field(p, 16, 8);
    msg->capacity = field(p, 24, 8);
    break;
  case 0x0a:
    msg->kind = P25_MOT_EMERG_ALRM;
    msg->group = field(p, 40, 16);
    msg->src = field(p, 56, 24);
    break;
  case 0x0b:
    msg->kind = P25_MOT_BSI;
    cwid(msg->cwid, p);
    msg->ch = chan(p, 64);
    break;
  case 0x0e:
    msg->kind = P25_MOT_CC_SHUTDOWN;
    msg->shutdown_min = field(p, 16, 16);
    break;
  case 0x16:
    msg->kind = P25_MOT_TDMA_DATA_CH;
    msg->ch = chan(p, 32);
    msg->ch_b = chan(p, 48);
    msg->has_b = true;
    break;
  default:
    break;
  }
}

static void tsbk_harris(struct p25_msg *msg, uint8_t const *p){
  if(msg->opcode != 0x30)
    return;
  // Group regroup explicit encryption command. Options: 0x80 simulselect (else patch),
  // 0x40 talkgroup target (else a radio), 0x20 activate, 0x1f supergroup sequence number
  msg->kind = P25_HARRIS_GRG_EXENC_CMD;
  msg->regroup_opts = field(p, 16, 8);
  msg->group = field(p, 24, 16);
  msg->key_id = field(p, 40, 16);
  msg->tgt = field(p, 56, 24);
  if(msg->regroup_opts & 0x40){ // a talkgroup target: algorithm, then the talkgroup
    msg->alg_id = field(p, 56, 8);
    msg->group_b = field(p, 64, 16);
  }
}

int p25_decode_tsbk(struct p25_msg *msg, uint8_t const tsbk[12]){
  memset(msg, 0, sizeof *msg);
  if(tsbk == NULL)
    return -1;
  msg->family = P25_FAMILY_TSBK;
  msg->last_block = bit(tsbk, 0);
  msg->protect = bit(tsbk, 1);
  msg->opcode = field(tsbk, 2, 6);
  msg->mfid = tsbk[1];
  keep_raw(msg, tsbk, 12);
  switch(msg->mfid){
  case P25_MFID_STD:
  case P25_MFID_STD1:
    tsbk_standard(msg, tsbk);
    break;
  case P25_MFID_MOTOROLA:
    tsbk_motorola(msg, tsbk);
    break;
  case P25_MFID_HARRIS:
    tsbk_harris(msg, tsbk);
    break;
  default: // another manufacturer's opcodes
    break;
  }
  return 0;
}

// Link control (TIA-102.AABF): bit 0 protected, bit 1 standard format (implicit MFID),
// opcode 2-7, MFID 8-15 when bit 1 is clear
static enum p25_kind lc_standard_kind(int opcode){
  switch(opcode){
  case 0x00: return P25_LC_GRP_V_CH_USR;
  case 0x02: return P25_GRP_V_CH_GRANT_UPDT;
  case 0x03: return P25_LC_UU_V_CH_USR;
  case 0x04: return P25_GRP_V_CH_GRANT_UPDT_EXP;
  case 0x05: return P25_UU_ANS_REQ;
  case 0x06: return P25_LC_TEL_INT_V_CH_USR;
  case 0x07: return P25_TELE_INT_ANS_REQ;
  case 0x09: return P25_LC_SRC_ID_EXT;
  case 0x0a: return P25_LC_UU_V_CH_USR_EXT;
  case 0x0f: return P25_LC_CALL_TRM;
  case 0x10: return P25_GRP_AFF_Q;
  case 0x11: return P25_U_REG_CMD;
  case 0x13: return P25_STS_Q;
  case 0x14: return P25_STS_UPDT;
  case 0x15: return P25_MSG_UPDT;
  case 0x16: return P25_CALL_ALRT;
  case 0x17: return P25_EXT_FNCT_CMD;
  case 0x20: return P25_SYS_SRV_BCST;
  case 0x21: return P25_SCCB;
  case 0x22: return P25_ADJ_STS_BCST;
  case 0x23: return P25_RFSS_STS_BCST;
  case 0x24: return P25_NET_STS_BCST;
  case 0x26: return P25_SCCB_EXP;
  case 0x27: return P25_ADJ_STS_BCST_EXP;
  case 0x28: return P25_RFSS_STS_BCST_EXP;
  case 0x29: return P25_NET_STS_BCST_EXP;
  // authentication, channel identifier updates, extended-source forms, protection parameters
  case 0x12: case 0x18: case 0x19: case 0x1a: case 0x1b: case 0x1c: case 0x25: case 0x2a:
    return P25_LC_OTHER;
  default: return P25_UNKNOWN;
  }
}
static enum p25_kind lc_motorola_kind(int opcode){
  switch(opcode){
  case 0x00: return P25_LC_MOT_GRG_V_CH_USR;
  case 0x01: return P25_LC_MOT_GRG_V_CH_UPDT;
  case 0x02: return P25_LC_MOT_FAILSOFT;
  case 0x03: return P25_MOT_GRG_ADD;
  case 0x04: return P25_MOT_GRG_DEL;
  case 0x06: return P25_LC_MOT_UNIT_GPS;
  case 0x0a: return P25_MOT_EMERG_ALRM;
  case 0x0f: return P25_LC_MOT_TALK_COMPLETE;
  case 0x15: return P25_LC_MOT_ALIAS_HDR;
  case 0x17: return P25_LC_MOT_ALIAS_DATA;
  default: return P25_UNKNOWN;
  }
}
static enum p25_kind lc_harris_kind(int opcode){
  switch(opcode){
  case 0x0a: return P25_LC_HARRIS_RETURN_CC;
  case 0x2a: return P25_LC_HARRIS_GPS1;
  case 0x2b: return P25_LC_HARRIS_GPS2;
  case 0x32: case 0x33: case 0x34: case 0x35: return P25_LC_HARRIS_ALIAS;
  default: return P25_UNKNOWN;
  }
}

int p25_decode_lc(struct p25_msg *msg, uint8_t const lc[9], bool terminator){
  memset(msg, 0, sizeof *msg);
  if(lc == NULL)
    return -1;
  uint8_t const *p = lc;
  msg->family = terminator ? P25_FAMILY_TDULC : P25_FAMILY_LC;
  msg->protect = bit(p, 0);
  msg->sf = bit(p, 1);
  msg->opcode = field(p, 2, 6);
  msg->mfid = msg->sf ? P25_MFID_STD : p[1];
  keep_raw(msg, p, 9);
  switch(msg->mfid){
  case P25_MFID_MOTOROLA:
    msg->kind = lc_motorola_kind(msg->opcode);
    break;
  case P25_MFID_HARRIS:
    msg->kind = lc_harris_kind(msg->opcode);
    break;
  case P25_MFID_STD:
  case P25_MFID_STD1:
    msg->kind = lc_standard_kind(msg->opcode);
    break;
  default:
    msg->kind = P25_UNKNOWN;
    break;
  }
  if(msg->protect)
    return 0; // the rest is encrypted

  switch(msg->kind){
  case P25_LC_GRP_V_CH_USR:
  case P25_LC_MOT_GRG_V_CH_USR:
    msg->svc = field(p, 16, 8);
    msg->group = field(p, 32, 16);
    msg->src = field(p, 48, 24);
    break;
  case P25_GRP_V_CH_GRANT_UPDT:
    msg->ch = chan(p, 8);
    msg->group = field(p, 24, 16);
    msg->ch_b = chan(p, 40);
    msg->group_b = field(p, 56, 16);
    msg->has_b = msg->ch_b.num != 0 && msg->group != msg->group_b;
    break;
  case P25_LC_UU_V_CH_USR:
    msg->svc = field(p, 16, 8);
    msg->tgt = field(p, 24, 24);
    msg->src = field(p, 48, 24);
    break;
  case P25_GRP_V_CH_GRANT_UPDT_EXP:
    msg->svc = field(p, 16, 8);
    msg->group = field(p, 24, 16);
    msg->ch = chan(p, 40);   // downlink
    msg->ch_b = chan(p, 56); // uplink
    msg->has_b = true;
    break;
  case P25_LC_TEL_INT_V_CH_USR:
    msg->svc = field(p, 16, 8);
    msg->call_timer_ms = field(p, 32, 16) * 100;
    msg->tgt = field(p, 48, 24);
    break;
  case P25_LC_UU_V_CH_USR_EXT: // service options in octet 1; the source is in an extension LC
    msg->svc = field(p, 8, 8);
    msg->tgt = field(p, 24, 24);
    break;
  case P25_LC_CALL_TRM:
  case P25_LC_MOT_TALK_COMPLETE:
    msg->address = field(p, 48, 24);
    break;
  case P25_UU_ANS_REQ:
  case P25_GRP_AFF_Q:
  case P25_CALL_ALRT:
    msg->tgt = field(p, 24, 24);
    msg->src = field(p, 48, 24);
    break;
  case P25_STS_UPDT:
    msg->unit_status = field(p, 8, 8);
    msg->user_status = field(p, 16, 8);
    msg->tgt = field(p, 24, 24);
    msg->src = field(p, 48, 24);
    break;
  case P25_MSG_UPDT:
    msg->message = field(p, 8, 16);
    msg->tgt = field(p, 24, 24);
    msg->src = field(p, 48, 24);
    break;
  case P25_LC_MOT_GRG_V_CH_UPDT:
    msg->svc = field(p, 16, 8);
    msg->group = field(p, 24, 16);
    msg->ch = chan(p, 56);
    break;
  case P25_SYS_SRV_BCST:
    msg->avail_svc = field(p, 24, 24);
    msg->supp_svc = field(p, 48, 24);
    break;
  case P25_SCCB:
    msg->rfss = field(p, 8, 8);
    msg->site = field(p, 16, 8);
    msg->ch = chan(p, 24);
    msg->svc_class = field(p, 40, 8);
    msg->ch_b = chan(p, 48);
    msg->svc_class_b = field(p, 64, 8);
    msg->has_b = msg->ch_b.num != 0;
    break;
  case P25_ADJ_STS_BCST:
  case P25_RFSS_STS_BCST:
    msg->lra = field(p, 8, 8);
    msg->sysid = field(p, 20, 12);
    msg->rfss = field(p, 32, 8);
    msg->site = field(p, 40, 8);
    msg->ch = chan(p, 48);
    msg->svc_class = field(p, 64, 8);
    break;
  case P25_NET_STS_BCST:
    msg->wacn = field(p, 16, 20);
    msg->sysid = field(p, 36, 12);
    msg->ch = chan(p, 48);
    msg->svc_class = field(p, 64, 8);
    break;
  case P25_SCCB_EXP:
    msg->rfss = field(p, 8, 8);
    msg->site = field(p, 16, 8);
    msg->ch = chan(p, 24);
    msg->ch_b = chan(p, 40);
    msg->has_b = msg->ch_b.num != 0;
    msg->svc_class = field(p, 56, 8);
    break;
  case P25_ADJ_STS_BCST_EXP:
    msg->lra = field(p, 8, 8);
    msg->ch = chan(p, 16);
    msg->rfss = field(p, 32, 8);
    msg->site = field(p, 40, 8);
    msg->ch_b = chan(p, 48);
    msg->has_b = msg->ch_b.num != 0;
    msg->svc_class = field(p, 64, 8);
    break;
  case P25_RFSS_STS_BCST_EXP: // uplink first
    msg->lra = field(p, 8, 8);
    msg->ch_b = chan(p, 16);
    msg->has_b = msg->ch_b.num != 0;
    msg->rfss = field(p, 32, 8);
    msg->site = field(p, 40, 8);
    msg->ch = chan(p, 48);
    msg->svc_class = field(p, 64, 8);
    break;
  case P25_NET_STS_BCST_EXP:
    msg->wacn = field(p, 8, 20);
    msg->sysid = field(p, 28, 12);
    msg->ch = chan(p, 40);
    msg->ch_b = chan(p, 56);
    msg->has_b = msg->ch_b.num != 0;
    break;
  case P25_MOT_GRG_ADD:
  case P25_MOT_GRG_DEL:
    msg->group = field(p, 16, 16);
    msg->members[0] = field(p, 32, 16);
    msg->members[1] = field(p, 48, 16);
    msg->nmembers = 2;
    break;
  case P25_LC_MOT_UNIT_GPS:
    {
      double const scale = 1.0 / 0x7fffff;
      msg->latitude = field(p, 25, 23) * 90.0 * scale * (bit(p, 24) ? -1.0 : 1.0);
      msg->longitude = field(p, 49, 23) * 180.0 * scale + (bit(p, 48) ? -180.0 : 0.0);
    }
    break;
  case P25_MOT_EMERG_ALRM:
    msg->group = field(p, 16, 16);
    msg->src = field(p, 48, 24);
    break;
  case P25_LC_MOT_ALIAS_HDR:
    msg->group = field(p, 16, 16);
    msg->block_count = field(p, 32, 8);
    msg->alias_format = field(p, 40, 8);
    msg->sequence = field(p, 56, 4);
    break;
  case P25_LC_MOT_ALIAS_DATA:
    msg->block = field(p, 16, 8);
    msg->sequence = field(p, 24, 4);
    break;
  case P25_LC_HARRIS_RETURN_CC:
    msg->src = field(p, 24, 24);
    msg->tgt = field(p, 48, 24);
    break;
  case P25_LC_HARRIS_ALIAS: // block 1-4, octets 2-8 are text
    msg->block = msg->opcode - 0x31;
    for(int i = 0; i < 7; i++){
      uint8_t c = p[2 + i];
      msg->text[i] = (c >= 0x20 && c < 0x7f) ? c : (c == 0 ? '\0' : '?');
    }
    msg->text[7] = '\0';
    break;
  default:
    break;
  }
  return 0;
}

// L3Harris talker GPS: octets 2-8 of block 1 then of block 2, 112 bits
void p25_harris_gps(uint8_t const block1[9], uint8_t const block2[9], double *latitude,
                    double *longitude, uint32_t *seconds_of_day, uint16_t *heading){
  uint8_t m[14];
  memcpy(m, block1 + 2, 7);
  memcpy(m + 7, block2 + 2, 7);
  double lat = field(m, 24, 8) + (field(m, 17, 7) + field(m, 0, 16) / 10000.0) / 60.0;
  double lon = field(m, 56, 8) + (field(m, 49, 7) + field(m, 32, 16) / 10000.0) / 60.0;
  if(latitude)
    *latitude = bit(m, 16) ? -lat : lat;
  if(longitude)
    *longitude = bit(m, 48) ? -lon : lon;
  if(seconds_of_day)
    *seconds_of_day = field(m, 64, 16) * (bit(m, 80) ? 2u : 1u);
  if(heading)
    *heading = field(m, 95, 9);
}

// Packet data (TIA-102.BAAA). Header: confirmed 1, outbound 2, format 3-7, SAP 10-15,
// MFID 16-23, LLID 24-47, blocks to follow 49-55, pad octets 59-63, data header offset 74-79
#define PDU_FMT_RESPONSE 0x03
#define PDU_FMT_UNCONFIRMED 0x15
#define PDU_FMT_CONFIRMED 0x16
#define PDU_FMT_AMBTC 0x17
#define SAP_SNDCP_DATA 4
#define SAP_SNDCP_CONTROL 6
#define SAP_TRUNKING 61
#define SAP_TRUNKING_ENC 63

// User octets of a packet: data block octets (a confirmed block's serial number and
// CRC-9 dropped) without the pad octets and the CRC-32; -1 if blocks are missing
static int user_data(uint8_t const *pdu, int len, struct p25_msg const *msg, uint8_t *out, int max){
  int const block = msg->confirmed ? 18 : 12;
  int const skip = msg->confirmed ? 2 : 0;
  if(msg->blocks == 0 || len < 12 + msg->blocks * block)
    return -1;
  int n = (block - skip) * msg->blocks - msg->pad_octets - 4;
  if(n <= 0)
    return -1;
  int k = 0;
  for(int b = 0; b < msg->blocks; b++)
    for(int o = skip; o < block && k < max && k < n; o++)
      out[k++] = pdu[12 + b * block + o];
  return n;
}

// SNDCP context control (TIA-102.BAEB), the first octets of the user data
static void sndcp_control(struct p25_msg *msg, uint8_t const *d, int n){
  if(n <= 0)
    return;
  msg->sndcp_type = d[0] >> 4;
  msg->sndcp_nsapi = d[0] & 0x0f;
  if(n > 1){
    msg->sndcp_has_arg = true;
    msg->sndcp_arg = d[1];
  }
  // the IPv4 address: an outbound accept at octet 3, an inbound request at octet 2
  int at = -1;
  if(msg->outbound && msg->sndcp_type == 0)
    at = 3;
  else if(!msg->outbound && msg->sndcp_type == 0 && n > 1)
    at = 2;
  if(at >= 0 && n >= at + 4){
    msg->sndcp_has_ip = true;
    memcpy(msg->sndcp_ip, d + at, 4);
  }
}

// Alternate multi-block trunking control (TIA-102.AABC): opcode 58-63 and service options
// 64-71 of the header; LLID is the source (or the unit); data block 0 from octet 12
// Only the opcodes below (standard, and Motorola's group regroup grant) are decoded; another
// keeps family MBT, kind P25_UNKNOWN, and is logged with its header
static void mbt(struct p25_msg *msg, uint8_t const *h){
  uint8_t const *b0 = h + 12;
  msg->opcode = field(h, 58, 6);
  msg->svc = field(h, 64, 8);
  if(msg->mfid == P25_MFID_MOTOROLA){
    if(msg->opcode == 0x02){
      msg->kind = P25_MOT_GRG_CH_GRANT;
      msg->ch = chan(b0, 0);
      msg->group = field(b0, 32, 16);
      msg->src = msg->llid;
    }
    return;
  }
  if(msg->mfid != P25_MFID_STD && msg->mfid != P25_MFID_STD1)
    return;
  switch(msg->opcode){
  case 0x00:
    msg->kind = P25_GRP_V_CH_GRANT;
    msg->ch = chan(b0, 16);
    msg->group = field(b0, 48, 16);
    msg->src = msg->llid;
    break;
  case 0x04:
  case 0x06:
    msg->kind = msg->opcode == 0x04 ? P25_UU_V_CH_GRANT : P25_UU_V_CH_GRANT_UPDT;
    msg->src = field(b0, 32, 24);
    msg->tgt = field(b0, 56, 24);
    msg->ch = chan(b0, 80);
    break;
  case 0x08:
  case 0x09:
    msg->kind = msg->opcode == 0x08 ? P25_TELE_INT_CH_GRANT : P25_TELE_INT_CH_GRANT_UPDT;
    msg->ch = chan(b0, 0);
    msg->timer = field(b0, 32, 16);
    msg->tgt = msg->llid;
    break;
  case 0x10:
    msg->kind = P25_IND_DATA_CH_GRANT;
    msg->ch = chan(b0, 80);
    msg->tgt = field(b0, 56, 24);
    break;
  case 0x11:
    msg->kind = P25_GRP_DATA_CH_GRANT;
    msg->ch = chan(b0, 16);
    msg->group = field(b0, 48, 16);
    break;
  case 0x3a:
    msg->kind = P25_RFSS_STS_BCST;
    msg->sysid = field(h, 36, 12);
    msg->rfss = field(b0, 0, 8);
    msg->site = field(b0, 8, 8);
    msg->ch = chan(b0, 16);
    break;
  case 0x3b:
    msg->kind = P25_NET_STS_BCST;
    msg->sysid = field(h, 36, 12);
    msg->wacn = field(b0, 0, 20);
    msg->ch = chan(b0, 24);
    break;
  case 0x3c:
    msg->kind = P25_ADJ_STS_BCST;
    msg->sysid = field(h, 36, 12);
    msg->rfss = field(h, 64, 8);
    msg->site = field(h, 72, 8);
    msg->ch = chan(b0, 0);
    break;
  default:
    break;
  }
}

// The header first, whatever the format; then by format. An AMBTC is decoded only on the
// trunking SAP and with its first data block present (len 24 or more)
int p25_decode_pdu(struct p25_msg *msg, uint8_t const *pdu, int len){
  memset(msg, 0, sizeof *msg);
  if(pdu == NULL || len < 12)
    return -1;
  msg->family = P25_FAMILY_PDU;
  msg->kind = P25_PDU_OTHER;
  keep_raw(msg, pdu, 12);
  msg->confirmed = bit(pdu, 1);
  msg->outbound = bit(pdu, 2);
  msg->format = field(pdu, 3, 5);
  msg->sap = field(pdu, 10, 6);
  msg->mfid = pdu[2];
  msg->llid = field(pdu, 24, 24);
  msg->blocks = field(pdu, 49, 7);
  msg->pad_octets = field(pdu, 59, 5);
  msg->header_offset = field(pdu, 74, 6);
  msg->user_octets = -1;

  switch(msg->format){
  case PDU_FMT_AMBTC:
    msg->family = P25_FAMILY_MBT;
    msg->kind = P25_UNKNOWN;
    msg->opcode = field(pdu, 58, 6);
    if(len >= 24 && msg->sap == SAP_TRUNKING)
      mbt(msg, pdu);
    break;
  case PDU_FMT_RESPONSE:
    msg->kind = P25_PDU_RESPONSE;
    msg->pdu_response = pdu[1];
    msg->has_src_llid = !bit(pdu, 48);
    msg->src_llid = field(pdu, 56, 24);
    break;
  case PDU_FMT_CONFIRMED:
  case PDU_FMT_UNCONFIRMED:
    {
      if(msg->sap == SAP_TRUNKING || msg->sap == SAP_TRUNKING_ENC)
        break; // UMBTC: trunking control, header only
      uint8_t d[16];
      msg->user_octets = user_data(pdu, len, msg, d, sizeof d);
      if(msg->sap == SAP_SNDCP_CONTROL && msg->mfid == P25_MFID_STD){
        msg->kind = P25_PDU_SNDCP;
        sndcp_control(msg, d, msg->user_octets);
      } else
        msg->kind = msg->sap == SAP_SNDCP_DATA ? P25_PDU_SNDCP_DATA : P25_PDU_DATA;
    }
    break;
  default:
    break;
  }
  return 0;
}

// HDU header word: MI 0-71, MFID 72-79, ALGID 80-87, KID 88-103, TGID 104-119
int p25_decode_hdu(struct p25_msg *msg, uint8_t const hdu[15]){
  memset(msg, 0, sizeof *msg);
  if(hdu == NULL)
    return -1;
  msg->family = P25_FAMILY_HDU;
  msg->kind = P25_HDU;
  keep_raw(msg, hdu, 15);
  memcpy(msg->mi, hdu, 9);
  msg->mfid = hdu[9];
  msg->algid = hdu[10];
  msg->kid = field(hdu, 88, 16);
  msg->group = field(hdu, 104, 16);
  return 0;
}

// LDU2 encryption sync: MI 0-71, ALGID 72-79, KID 80-95.
// One whose algorithm, key and message indicator are all zero is taken as unencrypted: algid
// is given as 0x80 and zero_es set (the raw octets are kept as sent). Any other ALGID 0x00 is
// an algorithm like the rest, so encrypted, and a header saying 0x00 is not excused. This is
// what sdrtrunk does (GPL-3, Dennis Sheirer; its issue 2013), because it was seen on the air:
// some L3Harris console transmissions that are not encrypted send such a sync, and without
// this a clear call is taken as encrypted
int p25_decode_es(struct p25_msg *msg, uint8_t const es[12]){
  memset(msg, 0, sizeof *msg);
  if(es == NULL)
    return -1;
  msg->family = P25_FAMILY_ES;
  msg->kind = P25_ENC_SYNC;
  keep_raw(msg, es, 12);
  memcpy(msg->mi, es, 9);
  msg->algid = es[9];
  msg->kid = field(es, 80, 16);
  static uint8_t const zeros[12];
  if(memcmp(es, zeros, sizeof zeros) == 0){
    msg->zero_es = true;
    msg->algid = 0x80;
  }
  return 0;
}
