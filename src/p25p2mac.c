// P25 Phase 2 MAC PDU parsing (TIA-102.BBAC, BBAD): walks a SACCH, FACCH or LCCH PDU into
// its structures and decodes them into struct p25_msg, with one-line log formatting
// Copyright 2026, Matthew Kaufman, KA6SQG

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "p25p1msg.h"
#include "p25p2mac.h"

// Theory of operation
//
// The PDU. A MAC PDU is what one SACCH, FACCH or LCCH burst carries once p25p2unit.c has
// corrected it and checked its CRC: 180 bits (22.5 octets) from a SACCH or an LCCH, 156 (19.5
// octets) from a FACCH. Every PDU starts with an 8-bit MAC PDU header: a 3-bit opcode (the PDU
// type; elsewhere in this file "opcode" means a structure's), a 3-bit offset, a reserved bit
// and the protected bit P (TIA-102.BBAD-A 2.1); a PDU with P set is discarded (p25_mac_decode()
// returns -1), as 4.3 has a receiver that cannot undo the protection do. A SACCH or FACCH PDU ends
// in a CRC-12, g(x) = x^12 + x^11 + x^7 + x^4 + x^2 + x + 1, sent inverted, over everything
// before it from the header on (TIA-102.BBAC 8.4.5): from bit 168 or bit 144. An outbound
// LCCH PDU ends instead in a 28-bit trailer, the site's 12-bit color code (its NAC, bits
// 152-163) and a CRC-16 over everything before it, G(x) = x^16 + x^12 + x^5 + 1, inverted
// (TIA-102.BBAD-A 2.1, 2.2, 4.7). The PDU types are 0 MAC_SIGNAL (the only LCCH PDU,
// TIA-102.BBAD-A 2.2.3), 1 MAC_PTT, 2 MAC_END_PTT, 3 MAC_IDLE, 4 MAC_ACTIVE and 6
// MAC_HANGTIME, with 5 and 7 reserved (TIA-102.BBAD-A 4.1). The offset counts the non-SACCH
// bursts on this timeslot before the first 4V burst of the next voice sequence (outbound, 0 to
// 5), 7 meaning no voice framing or none known (TIA-102.BBAD-A 4.2); it is logged, not used.
//
// PTT and END PTT have fixed layouts (TIA-102.BBAD-A 2.3.3.1, 2.3.3.2; positions in
// p25p2mac.h) and carry no structures. SIGNAL, IDLE, ACTIVE and HANGTIME carry MAC structures
// from bit 8 up to the CRC or the color code (max_index(); TIA-102.BBAD-A 2.2.3, 2.3.3.3);
// types 5 and 7 are taken no further. A PDU with no PTT reads as clear (ALGID 0x80); a PTT's
// ALGID 0x00 is an algorithm like any other, not clear.
//
// Walking the structures. A structure is an opcode octet and its fields, and nothing in the PDU
// marks where one ends. TIA-102.BBAD-A 3 gives four ways to find a structure's length: fixed
// by the opcode (its Table 4); from the structure's own fields (the two paging messages, by
// their LEN field, 3.37 and 3.38); the rest of the PDU (Null Information, 3.1); or a length
// octet, the low six bits of the octet after the opcode, which every message added after the
// first edition of the MAC specification carries so that a receiver can step over opcodes it
// does not know. A length counts every octet of the structure in this PDU, the opcode and the
// length octet included (4.11). A manufacturer's structure has its MFID in octet 1 and its
// length in the low six bits of octet 2 (3.36). struct_len() follows that with the tables
// below for the fixed lengths and for the vendors, LEN for paging (LEN + 1 IDs, Tables 6 and 7),
// the length octet for null avoid zero bias and multi-fragment first fragments and
// continuations, and for the manufacturer partition the length in octet 2 when that is
// plausible (3 or more and inside the PDU: not every one sets it), else the vendor table's. An
// opcode no table knows is stepped over by its length octet when that is plausible (2 or more,
// inside the PDU), and the walk goes on; otherwise it is taken to fill the rest of the PDU: a
// wrong guess at its length would misread every structure after it, while taking the rest
// loses only those. TIA-102.BBAD-A 3 puts a
// manufacturer's structures after the standard ones and Null Information (or Null Avoid Zero
// Bias Information) last, filling what is left, so a receiver that cannot size a structure
// loses nothing standard after it; on the LCCH more than one octet of fill is Null
// Information if the burst is scrambled and Null Avoid Zero Bias if not (2.2.3). The standard
// puts as many structures in a PDU as fit (2.2.3, 2.3.3.3), and so does this walk (up to
// P25_MAC_MAX_STRUCT: a SACCH PDU's 20 octets, no structure shorter than 2). A
// NULL INFORMATION opcode (0x00) after the first, or a length of 0, ends it, and a PDU whose
// first opcode is 0x00 has that one null structure.
//
// Partitions. The opcode's top two bits, B1 and B2, choose one of four sets (TIA-102.BBAD-A 3,
// Table 3), so the same 6-bit opcode means different things in each. 00 (0x00-0x3f):
// structures made for TDMA with no TSBK counterpart (voice channel user, the multiple grant
// updates, paging, power control, MAC release). 01 (0x40-0x7f): the abbreviated forms of the
// Phase 1 TSBKs, whose low six bits are the TSBK's opcode. 10 (0x80-0xbf): manufacturers'
// structures (TIA-102.BBAD-A 3.36), except 0x88 and 0x90, which are standard structures in
// that range. 11 (0xc0-0xff): the explicit and extended forms, with a separate uplink channel
// or a full SUID for a roaming unit: 20-bit WACN ID, 12-bit system ID and 24-bit unit ID,
// seven octets (TIA-102.BBAD-A 4.13, suid() below).
//
// Decoding. An abbreviated structure is a TSBK with its MFID, its CRC (the PDU's CRC covers it)
// and some reserved fields taken out, under the TSBK's own opcode (TIA-102.BBAD-A 2.2.3,
// 2.3.3.3, 3). One that keeps the TSBK's field order is rebuilt as that TSBK (as_tsbk(),
// positions in P1_at[]) and read by the Phase 1 decoder, so the structure becomes exactly the
// message the TSBK would, with the same kind, fields and log line. The rest are read here,
// field by field, into the same struct p25_msg (family P25_FAMILY_MAC, opcode the whole 8-bit
// MAC opcode), and what p25_msg has no place for (SUID, NAC, more than two grants, paging
// lists, aliases) goes in struct p25_mac_struct. Structures known by length only are
// P25_MAC_OTHER and logged raw.
//
// To the trunking logic. p25_mac_trunk_msgs() turns a structure into the messages the Phase 1
// path takes: a structure with a p25msg kind as it is (an explicit grant is the grant, with its
// uplink channel in ch_b), and the multiple group grant updates 0x05 and 0x25, which no TSBK
// has, as one single update per call. So the call tracking has no Phase 2 cases of its own.
// The other P25_MAC_ kinds (paging, power control, release, aliases ...) are for the log.
//
// Multi-fragment messages. A message too long for one LCCH PDU comes as a first fragment and
// continuations in the LCCH bursts that follow, its data ending in a Multi-Fragment CRC-16
// (TIA-102.BBAD-A 3, 4.7, 4.11, 4.12); p25_mac_frag_add() puts it together and parses it as one
// structure, which then goes the same way as the others.

// The PDU: octets MSB first, nbits of them valid. Bits past nbits read as zero.
struct pdu_bits {
  uint8_t const *p;
  int n;
};
static int get_bit(struct pdu_bits const *b, int i){
  if(i < 0 || i >= b->n)
    return 0;
  return (b->p[i >> 3] >> (7 - (i & 7))) & 1;
}
// A whole field, or zero if any of it lies past the PDU
static uint32_t get_field(struct pdu_bits const *b, int off, int len){
  if(off < 0 || off + len > b->n)
    return 0;
  uint32_t v = 0;
  for(int i = off; i < off + len; i++)
    v = (v << 1) | ((b->p[i >> 3] >> (7 - (i & 7))) & 1);
  return v;
}

// Structure lengths in octets, opcode included. 0: not listed (standard: its length octet when
// plausible, else the rest of the PDU;
// vendor: the vendor length field). VAR: variable, from the structure's own length field.
// REST: the rest of the PDU.
//
// Opcode lengths after sdrtrunk (MacOpcode.java, MacMessageFactory.java and the structure
// classes in module/decode/p25/phase2/message/mac), Dennis Sheirer, GPL-3; cross-checked
// against OP25 (gr-op25_repeater/lib/p25p2_tdma.cc mac_msg_len), Max H. Parke KA1RBI and
// Graham J. Norbury, GPL-3. Where they differ the length that the structure's fields need
// is taken: 0x45, 0x4a and 0xc5 are OP25's, as sdrtrunk's own field layouts for them end at
// 8, 9 and 12 octets, as TIA-102.BBAD-A Table 4 has them. 0xe3, 0xed, 0xf6 and 0xf8 are OP25's
// alone (inbound messages in Table 4). The MFID90 extended function command is 0x84, 11 octets
// (BBAD-A 3.88, Table 4; sdrtrunk's value 0x85 disagrees with the standard and its own name).
// Real Motorola traffic carries a 0x85 the standard does not list: its length comes from its
// length octet, its contents are not decoded. Multi-fragment first fragments (0x71,
// 0xc7 ...) are at most 18 octets, what an LCCH PDU holds, and their own length octet says;
// OP25 lists the whole message's length there instead (see p25_mac_frag_add()).
// The standard's list of lengths is TIA-102.BBAD-A 3, Table 4. An outbound LCCH PDU holds
// octets 1-18 of structures (TIA-102.BBAD-A 2.2.1), the 18 above.
#define VAR 0xff
#define REST 0xfe
static const uint8_t Std_len[256] = {
  [0x00] = REST, [0x01] = 7, [0x02] = 8, [0x03] = 7, [0x05] = 16, [0x08] = VAR,
  [0x10] = VAR, [0x11] = VAR, [0x12] = VAR, [0x21] = 14, [0x22] = 15, [0x25] = 15,
  [0x30] = 5, [0x31] = 7,
  [0x40] = 9, [0x41] = 7, [0x42] = 9, [0x44] = 9, [0x45] = 8, [0x46] = 9, [0x48] = 10,
  [0x49] = 10, [0x4a] = 9, [0x4c] = 10, [0x52] = 8, [0x53] = 9, [0x54] = 9, [0x55] = 7,
  [0x58] = 10, [0x5a] = 7, [0x5c] = 10, [0x5d] = 8, [0x5e] = 14, [0x5f] = 7, [0x60] = 9,
  [0x61] = 9, [0x64] = 9, [0x67] = 9, [0x68] = 10, [0x6a] = 7, [0x6b] = 10, [0x6c] = 10,
  [0x6d] = 7, [0x6f] = 9, [0x70] = 9, [0x71] = 18, [0x72] = 9, [0x73] = 9, [0x74] = 9,
  [0x75] = 9, [0x76] = 10, [0x77] = 13, [0x78] = 9, [0x79] = 9, [0x7a] = 9, [0x7b] = 11,
  [0x7c] = 9, [0x7d] = 9,
  [0x88] = 5, [0x90] = 7,  // standard-vendor opcodes in the vendor partition
  [0xc0] = 11, [0xc3] = 8, [0xc4] = 15, [0xc5] = 12, [0xc6] = 15, [0xc7] = 18, [0xc8] = 12,
  [0xc9] = 12, [0xcb] = 18, [0xcc] = 14, [0xcd] = 18, [0xce] = 18, [0xcf] = 18, [0xd6] = 9,
  [0xd8] = 14, [0xd9] = 18, [0xda] = 11, [0xdb] = 18, [0xdc] = 14, [0xde] = 18, [0xdf] = 11,
  [0xe0] = 18, [0xe3] = 14, [0xe4] = 17, [0xe5] = 14, [0xe8] = 16, [0xe9] = 8, [0xea] = 11,
  [0xec] = 13, [0xed] = 19, [0xf2] = 16, [0xf3] = 14, [0xf6] = 12, [0xf8] = 22, [0xfa] = 11,
  [0xfb] = 13, [0xfc] = 11, [0xfe] = 15,
};
// Vendor partition (0x80-0xbf), indexed by opcode - 0x80
static const uint8_t Motorola_len[64] = {
  [0x00] = 8, [0x01] = 17, [0x03] = 7, [0x04] = 11, [0x09] = 17, [0x11] = 17, [0x15] = 17,
  [0x20] = 16, [0x23] = 11, [0x24] = 13, [0x25] = 11, [0x26] = 11, [0x27] = 11, [0x28] = 10,
};
static const uint8_t Harris_len[64] = {
  [0x20] = 9, [0x28] = REST, [0x2a] = 17, [0x2c] = 12,
};

bool p25_mac_multi_frag(uint8_t opcode);

static bool vendor_partition(uint8_t op){
  return op >= 0x80 && op <= 0xbf && op != 0x88 && op != 0x90;
}

// Bits before the CRC: where structures may lie
static int max_index(enum p25_mac_chan chan){
  return chan == P25_MAC_FACCH ? 144 : chan == P25_MAC_LCCH ? 152 : 168;
}

// A manufacturer-specific structure carries its own length (octets, bits 18-23), but not
// every one sets it: the field is taken when it is plausible (3 or more octets, inside the
// PDU), else the vendor table's length
static int struct_len(struct pdu_bits const *b, int maxidx, int offset, uint8_t op, uint8_t *vendor){
  int const remaining = (maxidx - offset + 7) / 8;
  int len;
  *vendor = 0;
  if(vendor_partition(op)){
    *vendor = get_field(b, offset + 8, 8);
    int const lf = get_field(b, offset + 18, 6);
    uint8_t const *tbl = *vendor == P25_MFID_MOTOROLA ? Motorola_len : *vendor == P25_MFID_HARRIS ? Harris_len : NULL;
    int fixed = 0;
    if(tbl != NULL && tbl[op - 0x80] != 0 && tbl[op - 0x80] != VAR)
      fixed = tbl[op - 0x80] == REST ? -1 : tbl[op - 0x80];
    // Its length octet counts the opcode, MFID and length octets (BBAD-A 3.36): fewer than 3, or
    // more than the PDU holds, cannot be right; then the table's length, else the rest of the PDU
    if(lf >= 3 && lf <= remaining)
      len = lf;
    else
      len = fixed != 0 ? fixed : -1;
  } else {
    uint8_t const t = Std_len[op];
    if(t == REST)
      len = -1;
    else if(t == 0){
      // Not in the table. Every message added since the standard's first publication carries its
      // length in the low 6 bits of its second octet, so that older units can skip it (BBAD-A
      // 3): taken when plausible, and the walk goes on; otherwise the rest of the PDU
      int const lf = get_field(b, offset + 10, 6);
      len = lf >= 2 && lf <= remaining ? lf : -1;
    } else if(p25_mac_multi_frag(op)){
      // The first fragment of a multi-fragment message: its own length octet (BBAD-A 4.11); the
      // table's 18 is the most an outbound LCCH PDU holds
      int const lf = get_field(b, offset + 10, 6);
      len = lf >= 3 && lf <= remaining ? lf : t;
    } else if(t == VAR){
      // Paging: LEN (bits 14-15) + 1 IDs, 2-octet groups or 3-octet units (BBAD-A 3.37 and 3.38,
      // Tables 6 and 7); the others carry their length in bits 10-15
      int const count = get_field(b, offset + 14, 2) + 1;
      if(op == 0x11)
        len = 2 + 2 * count;
      else if(op == 0x12)
        len = 2 + 3 * count;
      else
        len = get_field(b, offset + 10, 6);
    } else
      len = t;
  }
  if(len < 0)
    len = remaining;
  return len;
}

int p25_mac_struct_len(uint8_t const *octets, int nbits, enum p25_mac_chan chan, int offset){
  struct pdu_bits b = { octets, nbits };
  if(octets == NULL || offset + 8 > nbits)
    return 0;
  uint8_t vendor;
  return struct_len(&b, max_index(chan), offset, get_field(&b, offset, 8), &vendor);
}

// Field readers relative to the structure being decoded
struct ctx {
  struct pdu_bits const *b;
  struct p25_mac_struct *s;
};
static uint32_t F(struct ctx const *c, int off, int len){
  return get_field(c->b, c->s->offset + off, len);
}
static bool B(struct ctx const *c, int off){
  return get_field(c->b, c->s->offset + off, 1);
}
static struct p25_chan CH(struct ctx const *c, int off){
  struct p25_chan ch = { .iden = F(c, off, 4), .num = F(c, off + 4, 12) };
  return ch;
}
static void suid(struct ctx const *c, int off){
  c->s->msg.wacn = F(c, off, 20);
  c->s->msg.sysid = F(c, off + 20, 12);
  c->s->suid = F(c, off + 32, 24);
  c->s->has_suid = true;
}

// A structure laid out as a TSBK: TSBK bits 16-79 (after opcode and MFID) are structure bits
// at-(at+63), each bit copied that lies inside the structure and the PDU. Then the Phase 1
// decoder reads it. Most abbreviated (partition 1) structures are the TSBK without its MFID
// octet (at 8); the 10-octet ones keep the TSBK's positions (at 16) and the 7-octet ones drop
// its two reserved octets after the opcode (at -8); vendor structures put it after the
// vendor and length octets (at 24).
static void as_tsbk(struct ctx const *c, int at, uint8_t tsbk_opcode, uint8_t mfid){
  uint8_t t[12] = { 0x80 | (tsbk_opcode & 0x3f), mfid };
  int const end = c->s->len * 8;
  for(int i = 16; i < 80; i++){
    int const k = at + i - 16;
    if(k >= 0 && k < end && get_bit(c->b, c->s->offset + k))
      t[i >> 3] |= 0x80 >> (i & 7);
  }
  p25_decode_tsbk(&c->s->msg, t);
  c->s->msg.last_block = false;
}

// Partition 1 (0x40-0x7f): where TSBK bit 16 lies in the structure; 0 means decoded here
static const int8_t P1_at[64] = {
  [0x00] = 8, [0x02] = 8, [0x04] = 8, [0x06] = 8, [0x14] = 8, [0x18] = 16,
  [0x1a] = -8, [0x1c] = 16, [0x1f] = -8, [0x20] = 8, [0x21] = 8, [0x24] = 8, [0x27] = 8,
  [0x28] = 16, [0x2a] = -8, [0x2b] = 16, [0x2c] = 16, [0x2d] = 8, [0x2f] = 8, [0x30] = 8,
  [0x32] = 8, [0x33] = 8, [0x34] = 8, [0x37] = 8, [0x38] = 8, [0x39] = 8,
  [0x3a] = 8, [0x3b] = 8, [0x3c] = 8, [0x3d] = 8,
};

static void grant(struct ctx const *c, int svc, int ch, int ch_ul, int group, bool distinct_all){
  struct p25_mac_struct *s = c->s;
  struct p25_mac_grant g = {
    .svc = svc >= 0 ? F(c, svc, 8) : 0,
    .ch = CH(c, ch),
    .group = F(c, group, 16),
  };
  if(ch_ul >= 0)
    g.ch_ul = CH(c, ch_ul);
  // the first always; a later one only if it names another, nonzero group
  if(s->ngrants > 0){
    if(g.group == 0)
      return;
    for(int i = 0; i < (distinct_all ? s->ngrants : 1); i++)
      if(s->grant[i].group == g.group)
        return;
  }
  s->grant[s->ngrants++] = g;
}
// The grants into the flat fields too: the first, and the second as the "b" call
static void grants_to_msg(struct p25_mac_struct *s){
  s->msg.svc = s->grant[0].svc;
  s->msg.ch = s->grant[0].ch;
  s->msg.group = s->grant[0].group;
  if(s->ngrants > 1){
    s->msg.ch_b = s->grant[1].ch;
    s->msg.group_b = s->grant[1].group;
    s->msg.has_b = true;
  }
}

static void harris_alias(struct ctx const *c){
  // From octet 3 to the end of the structure, up to a zero octet, blanks trimmed
  struct p25_mac_struct *s = c->s;
  int n = 0;
  for(int i = 24; i + 8 <= s->len * 8 && n < (int)sizeof s->alias - 1; i += 8){
    if(s->offset + i + 8 > c->b->n)
      break;
    uint8_t ch = F(c, i, 8);
    if(ch == 0)
      break;
    s->alias[n++] = ch;
  }
  while(n > 0 && (uint8_t)s->alias[n - 1] <= ' ')
    n--;
  int start = 0;
  while(start < n && (uint8_t)s->alias[start] <= ' ')
    start++;
  memmove(s->alias, s->alias + start, n - start);
  s->alias[n - start] = '\0';
}

// The standard structures (every partition but the manufacturers'). An abbreviated one with a
// TSBK's layout goes through as_tsbk(); 0x7b (network status) adds the NAC the TSBK has no room
// for, and 0x42 (grant update) also fills the grant list so that all grant updates read alike.
// An abbreviated opcode in neither table is still read as its TSBK, which is the best guess
// there is. The rest are laid out here, at the bit positions within the structure, opcode at 0
static void decode_standard(struct ctx const *c){
  struct p25_mac_struct *s = c->s;
  struct p25_msg *m = &s->msg;
  uint8_t const op = s->opcode;
  if(op >= 0x40 && op <= 0x7f && P1_at[op - 0x40] != 0){
    as_tsbk(c, P1_at[op - 0x40], op & 0x3f, P25_MFID_STD);
    if(op == 0x7b){
      s->nac = F(c, 76, 12);
      s->has_nac = true;
    }
    if(op == 0x42){ // the grant list as the other updates have it
      grant(c, -1, 8, -1, 24, false);
      grant(c, -1, 40, -1, 56, false);
    }
    return;
  }
  if(op >= 0x40 && op <= 0x7f && Std_len[op] == 0){
    // Not in either table: read as the TSBK it abbreviates; its length is unknown
    as_tsbk(c, 8, op & 0x3f, P25_MFID_STD);
    return;
  }
  switch(op){
  case 0x00:
    s->mac_kind = P25_MAC_NULL;
    break;
  case 0x01:
  case 0x21:
    m->kind = P25_LC_GRP_V_CH_USR;
    m->svc = F(c, 8, 8);
    m->group = F(c, 16, 16);
    m->src = F(c, 32, 24);
    if(op == 0x21)
      suid(c, 56);
    break;
  case 0x02:
  case 0x22:
    m->kind = P25_LC_UU_V_CH_USR;
    m->svc = F(c, 8, 8);
    m->tgt = F(c, 16, 24);
    m->src = F(c, 40, 24);
    if(op == 0x22)
      suid(c, 64);
    break;
  case 0x03:
    m->kind = P25_LC_TEL_INT_V_CH_USR;
    m->svc = F(c, 8, 8);
    m->call_timer_ms = F(c, 16, 16) * 100;
    m->tgt = F(c, 32, 24);
    break;
  case 0x05: // three of: service options, channel, group
    s->mac_kind = P25_MAC_GRP_V_CH_GRANT_UPDT_MULT;
    for(int i = 0; i < 3; i++)
      grant(c, 8 + 40 * i, 16 + 40 * i, -1, 32 + 40 * i, true);
    grants_to_msg(s);
    break;
  case 0x25: // two of: service options, downlink, uplink, group
    s->mac_kind = P25_MAC_GRP_V_CH_GRANT_UPDT_MULT_EXP;
    for(int i = 0; i < 2; i++)
      grant(c, 8 + 56 * i, 16 + 56 * i, 32 + 56 * i, 48 + 56 * i, false);
    grants_to_msg(s);
    break;
  case 0x08:
    s->mac_kind = P25_MAC_NULL_AZB;
    break;
  case 0x10:
    s->mac_kind = P25_MAC_MULTI_FRAG;
    break;
  case 0x11:
  case 0x12: // LEN 14-15 (that many IDs less one), then 16-bit groups or 24-bit units
    {
      s->mac_kind = op == 0x11 ? P25_MAC_GRP_PAGING : P25_MAC_IND_PAGING;
      int const w = op == 0x11 ? 16 : 24;
      for(int i = 0; i <= (int)F(c, 14, 2) && 16 + w * (i + 1) <= s->len * 8; i++)
        s->paged[s->npaged++] = F(c, 16 + w * i, w);
    }
    break;
  case 0x30:
    s->mac_kind = P25_MAC_PWR_CTL;
    m->tgt = F(c, 8, 24);
    s->rf_level = F(c, 32, 4);
    s->ber = F(c, 36, 4);
    break;
  case 0x31:
    s->mac_kind = P25_MAC_RELEASE;
    s->forced = B(c, 8);
    s->audio = B(c, 9);
    m->tgt = F(c, 16, 24);
    s->nac = F(c, 44, 12);
    s->has_nac = true;
    break;
  // Partition 1, where the TSBK layout does not carry over
  case 0x45: // no reserved octet: service options, target, source
    m->kind = P25_UU_ANS_REQ;
    m->svc = F(c, 8, 8);
    m->tgt = F(c, 16, 24);
    m->src = F(c, 40, 24);
    break;
  case 0x48:
  case 0x49:
    m->kind = op == 0x48 ? P25_TELE_INT_CH_GRANT : P25_TELE_INT_CH_GRANT_UPDT;
    m->svc = F(c, 16, 8); // octet 2 is the length (TIA-102.BBAD-A 3.44, 3.45)
    m->ch = CH(c, 24);
    m->timer = F(c, 40, 16);
    m->tgt = F(c, 56, 24);
    break;
  case 0x4a: // ten 4-bit digits, then the target
    m->kind = P25_TELE_INT_ANS_REQ;
    for(int i = 0; i < 10; i++)
      s->digits[i] = "0123456789abcdef"[F(c, 8 + 4 * i, 4)];
    m->tgt = F(c, 48, 24);
    break;
  case 0x4c: // radio unit monitor command (BBAD-A 3.43, Figure 99): length, TX time 16, SM 24,
             // TX multiplier 30, target 32, source 56; not the TSBK 0x0c layout (the old TSBK
             // form, 0x1d, is 0x5d, "obsolete")
    m->kind = P25_RAD_MON_CMD;
    m->tx_mult = F(c, 30, 2);
    m->tgt = F(c, 32, 24);
    m->src = F(c, 56, 24);
    break;
  case 0x55:
    m->kind = P25_SN_DATA_PAGE_REQ;
    m->svc = F(c, 8, 8);
    m->data_access = F(c, 16, 16);
    m->tgt = F(c, 32, 24);
    break;
  // TIA-102.BBAD-A 3.23: validity flags VD, VT, VL 8-10, reserved 11, local time offset
  // 12-23, date 24-47, time 48-71 (bits from the opcode's first)
  case 0x75: // month, day and year from bit 24; hour, minute and second from bit 48
    m->kind = P25_TIME_DATE_ANN;
    m->month = F(c, 24, 4);
    m->day = F(c, 28, 5);
    m->year = F(c, 33, 13);
    m->hour = F(c, 48, 5);
    m->minute = F(c, 53, 6);
    s->second = F(c, 59, 6);
    break;
  case 0x76:
    m->kind = P25_ROAM_ADDR_CMD;
    m->wacn = F(c, 24, 20);
    m->sysid = F(c, 44, 12);
    m->tgt = F(c, 56, 24);
    break;
  case 0x41: case 0x52: case 0x53: case 0x5d: case 0x5e: case 0x88:
    s->mac_kind = P25_MAC_OTHER;
    break;
  case 0x90:
    s->mac_kind = P25_MAC_GRG_V_CH_USR;
    m->group = F(c, 16, 16);
    m->src = F(c, 32, 24);
    break;
  // Extended partition: explicit channels, extended addresses
  case 0xc0:
    m->kind = P25_GRP_V_CH_GRANT;
    m->svc = F(c, 8, 8);
    m->ch = CH(c, 16);
    m->ch_b = CH(c, 32);
    m->has_b = true;
    m->group = F(c, 48, 16);
    m->src = F(c, 64, 24);
    break;
  case 0xc3:
    m->kind = P25_GRP_V_CH_GRANT_UPDT_EXP;
    grant(c, 8, 16, 32, 48, false);
    m->svc = s->grant[0].svc;
    m->ch = s->grant[0].ch;
    m->ch_b = s->grant[0].ch_ul;
    m->has_b = true;
    m->group = s->grant[0].group;
    break;
  case 0xc4:
  case 0xc6:
    m->kind = op == 0xc4 ? P25_UU_V_CH_GRANT : P25_UU_V_CH_GRANT_UPDT;
    m->ch = CH(c, 8);
    m->ch_b = CH(c, 24);
    m->has_b = true;
    suid(c, 40);
    m->src = s->suid;
    m->tgt = F(c, 96, 24);
    break;
  case 0xc8:
  case 0xc9:
    m->kind = op == 0xc8 ? P25_TELE_INT_CH_GRANT : P25_TELE_INT_CH_GRANT_UPDT;
    m->svc = F(c, 16, 8); // octet 2 is the length (BBAD-A 3.44, 3.45)
    m->ch = CH(c, 24);
    m->ch_b = CH(c, 40);
    m->has_b = true;
    m->timer = F(c, 56, 16);
    m->tgt = F(c, 72, 24);
    break;
  case 0xd6:
    m->kind = P25_SN_DATA_CH_ANN_EXP;
    m->svc = F(c, 8, 8);
    m->autonomous = B(c, 16);
    m->requested = B(c, 17);
    m->ch = CH(c, 24);
    m->ch_b = CH(c, 40);
    m->has_b = true;
    m->data_access = F(c, 56, 16);
    break;
  case 0xd8:
    m->kind = P25_STS_UPDT;
    m->unit_status = F(c, 16, 8);
    m->user_status = F(c, 24, 8);
    m->tgt = F(c, 32, 24);
    suid(c, 56);
    m->src = s->suid;
    break;
  case 0xda:
  case 0xdf:
  case 0xea:
    m->kind = op == 0xda ? P25_STS_Q : op == 0xdf ? P25_CALL_ALRT : P25_GRP_AFF_Q;
    m->tgt = F(c, 8, 24);
    suid(c, 32);
    m->src = s->suid;
    break;
  case 0xdc:
    m->kind = P25_MSG_UPDT;
    m->message = F(c, 16, 16);
    m->tgt = F(c, 32, 24);
    suid(c, 56);
    m->src = s->suid;
    break;
  case 0xe8: // group ID: WACN, system, group 56-103
    m->kind = P25_GRP_AFF_RSP;
    m->local_global = B(c, 16);
    m->response = F(c, 22, 2);
    m->group_b = F(c, 24, 16);
    m->group = F(c, 40, 16);
    m->wacn = F(c, 56, 20);
    m->sysid = F(c, 76, 12);
    m->tgt = F(c, 104, 24);
    break;
  case 0xe9:
    m->kind = P25_SCCB_EXP;
    m->rfss = F(c, 8, 8);
    m->site = F(c, 16, 8);
    m->ch = CH(c, 24);
    m->ch_b = CH(c, 40);
    m->has_b = m->ch_b.num != 0;
    m->svc_class = F(c, 56, 8);
    break;
  case 0xec:
    m->kind = P25_U_REG_RSP;
    m->response = F(c, 22, 2);
    suid(c, 24);
    m->info = s->suid;
    m->src = F(c, 80, 24);
    break;
  case 0xf3: // the TDMA identifier update at TSBK positions, then WACN and system
    as_tsbk(c, 16, 0x33, P25_MFID_STD);
    m->wacn = F(c, 80, 20);
    m->sysid = F(c, 100, 12);
    break;
  case 0xfa:
  case 0xfb:
  case 0xfc:
  case 0xfe:
    if(op == 0xfe){
      // Adjacent status, extended explicit (BBAD-A 3.18, Figure 55): a length octet, so every
      // field is one octet later than in 0xfc, then the WACN
      m->kind = P25_ADJ_STS_BCST_EXP;
      m->lra = F(c, 16, 8);
      m->site_flags = F(c, 24, 4);
      m->sysid = F(c, 28, 12);
      m->rfss = F(c, 40, 8);
      m->site = F(c, 48, 8);
      m->ch = CH(c, 56);
      m->ch_b = CH(c, 72);
      m->has_b = true;
      m->svc_class = F(c, 88, 8);
      m->wacn = F(c, 96, 20);
      break;
    }
    m->lra = F(c, 8, 8);
    m->ch = CH(c, 48);
    m->ch_b = CH(c, 64);
    m->has_b = true;
    m->svc_class = F(c, 80, 8);
    if(op == 0xfb){
      m->kind = P25_NET_STS_BCST_EXP;
      m->wacn = F(c, 16, 20);
      m->sysid = F(c, 36, 12);
      s->nac = F(c, 92, 12);
      s->has_nac = true;
      break;
    }
    m->kind = op == 0xfa ? P25_RFSS_STS_BCST_EXP : P25_ADJ_STS_BCST_EXP;
    m->site_flags = op == 0xfa ? B(c, 19) : F(c, 16, 4);
    m->sysid = F(c, 20, 12);
    m->rfss = F(c, 32, 8);
    m->site = F(c, 40, 8);
    break;
  case 0x71: case 0xc5: case 0xc7: case 0xcb: case 0xcc: case 0xcd: case 0xce: case 0xcf:
  case 0xd9: case 0xdb: case 0xde: case 0xe0: case 0xe3: case 0xe4: case 0xe5: case 0xed:
  case 0xf2: case 0xf6: case 0xf8:
    s->mac_kind = P25_MAC_OTHER;
    break;
  default:
    break;
  }
}

// Bits [off, off+n) of the PDU from the structure, past the structure's end if need be
static void fragment(struct ctx const *c, int off, int n){
  struct p25_mac_struct *s = c->s;
  memset(s->frag, 0, sizeof s->frag);
  for(int i = 0; i < n; i++)
    if(get_bit(c->b, s->offset + off + i))
      s->frag[i >> 3] |= 0x80 >> (i & 7);
  s->frag_bits = n;
}

// Motorola structures (MFID 0x90): opcode, MFID, length, then the fields from bit 24. Several
// are a Motorola TSBK after those three octets and go through as_tsbk() with Motorola's MFID
static void decode_motorola(struct ctx const *c){
  struct p25_mac_struct *s = c->s;
  struct p25_msg *m = &s->msg;
  switch(s->opcode){
  case 0x80: // MFID 8, then octet 3: reserved, P (protected: an encrypted call), reserved
             // (BBAD-A 3.86, Figure 178): the encryption as service options would give it
    m->kind = P25_LC_MOT_GRG_V_CH_USR;
    m->svc = B(c, 17) ? P25_SVC_ENCRYPTED : 0;
    m->group = F(c, 24, 16);
    m->src = F(c, 40, 24);
    break;
  case 0xa0:
    m->kind = P25_LC_MOT_GRG_V_CH_USR;
    m->svc = F(c, 24, 8);
    m->group = F(c, 32, 16);
    m->src = F(c, 48, 24);
    suid(c, 72);
    break;
  case 0x81:
  case 0x89: // supergroup, then as many talkgroups as the length holds, up to six
    {
      m->kind = s->opcode == 0x81 ? P25_MOT_GRG_ADD : P25_MOT_GRG_DEL;
      m->group = F(c, 24, 16);
      int n = (s->len - 5) / 2;
      n = n < 0 ? 0 : n > 6 ? 6 : n;
      for(int i = 0; i < n; i++)
        s->members[i] = F(c, 40 + 16 * i, 16);
      s->nmembers = n;
      m->nmembers = n; // all of them to the trunking logic (BBAD-A 3.92: up to six)
      memcpy(m->members, s->members, m->nmembers * sizeof m->members[0]);
    }
    break;
  case 0x83:
    m->kind = P25_LC_MOT_GRG_V_CH_UPDT;
    m->group = F(c, 24, 16);
    m->ch = CH(c, 40);
    break;
  case 0x84: // the extended function command TSBK after the vendor octets (BBAD-A 3.88)
    as_tsbk(c, 24, 0x04, P25_MFID_MOTOROLA);
    break;
  case 0x91: // talkgroup, blocks, format, sequence; the 64-bit fragment from 72 starts with the SUID
    m->kind = P25_LC_MOT_ALIAS_HDR;
    m->group = F(c, 24, 16);
    m->block_count = F(c, 40, 8);
    m->alias_format = F(c, 48, 8);
    m->sequence = F(c, 64, 4);
    suid(c, 72);
    fragment(c, 72, 64);
    break;
  case 0x95: // block, sequence; a 100-bit fragment from 36
    m->kind = P25_LC_MOT_ALIAS_DATA;
    m->block = F(c, 24, 8);
    m->sequence = F(c, 32, 4);
    fragment(c, 36, 100);
    break;
  case 0xa3:
    as_tsbk(c, 24, 0x02, P25_MFID_MOTOROLA);
    break;
  case 0xa4:
    m->kind = P25_MOT_GRG_CH_GRANT;
    m->svc = F(c, 24, 8);
    m->ch = CH(c, 32);
    m->ch_b = CH(c, 48);
    m->has_b = true;
    m->group = F(c, 64, 16);
    m->src = F(c, 80, 24);
    break;
  case 0xa5:
    as_tsbk(c, 24, 0x03, P25_MFID_MOTOROLA);
    break;
  case 0xa6:
  case 0xa7:
    as_tsbk(c, 24, s->opcode == 0xa6 ? 0x06 : 0x07, P25_MFID_MOTOROLA);
    break;
  case 0xa8:
    m->kind = P25_MOT_ACK_RSP;
    m->aiv = B(c, 24);
    m->ex = B(c, 25);
    m->svc_type = F(c, 26, 6);
    m->src = F(c, 32, 24);
    m->tgt = F(c, 56, 24);
    break;
  case 0x82: case 0x87: case 0x8b: case 0x8f: case 0xbf:
    s->mac_kind = P25_MAC_OTHER;
    break;
  default:
    break;
  }
}

static void decode_harris(struct ctx const *c){
  struct p25_mac_struct *s = c->s;
  struct p25_msg *m = &s->msg;
  switch(s->opcode){
  case 0xa0:
  case 0xac:
    s->mac_kind = P25_MAC_HARRIS_DATA_GRANT;
    m->ch = CH(c, 32);
    m->tgt = F(c, 48, 24);
    if(s->opcode == 0xac)
      m->src = F(c, 72, 24);
    break;
  case 0xa8:
    s->mac_kind = P25_MAC_HARRIS_ALIAS;
    harris_alias(c);
    break;
  case 0xb0: // the TSBK layout after the vendor octets; a talkgroup target may list more
    as_tsbk(c, 24, 0x30, P25_MFID_HARRIS);
    if(m->regroup_opts & 0x40)
      for(int i = 0; i < 4 && 88 + 16 * i <= s->len * 8; i++)
        s->members[s->nmembers++] = F(c, 72 + 16 * i, 16);
    break;
  case 0x81: case 0x8f: case 0xaa:
    s->mac_kind = P25_MAC_OTHER;
    break;
  default:
    break;
  }
}

// A multi-fragment message, reassembled (p25_mac_frag_add()): its first fragment's 18 octets,
// then each continuation's octets after its opcode and length octets, so a field the sources
// place at bit X of the continuation is at X + 128 here. Field positions after sdrtrunk's
// structure classes (module/decode/p25/phase2/message/mac/structure, Dennis Sheirer, GPL-3).
// Every one carries a source SUID; only the fields p25msg has room for are kept
static void decode_multi(struct ctx const *c){
  struct p25_mac_struct *s = c->s;
  struct p25_msg *m = &s->msg;
  switch(s->opcode){
  case 0xc7: // unit-to-unit voice channel grant update, extended (LCCH)
  case 0xcf: // unit-to-unit voice service channel grant, extended (LCCH)
    // Service options 24, source 32, source SUID 56, transmit channel 112, receive channel 128;
    // continuation: target 16 (here 144), target SUID 40 (168), multi-fragment CRC 96 (224).
    // sdrtrunk's 0xc7 class puts the continuation's target SUID at 56 and the receive channel
    // number at 128; the 0xcf layout is taken for both (OP25's length table gives both 32 octets,
    // which is where the 0xcf layout ends)
    m->kind = s->opcode == 0xcf ? P25_UU_V_CH_GRANT : P25_UU_V_CH_GRANT_UPDT;
    m->svc = F(c, 24, 8);
    m->src = F(c, 32, 24);
    suid(c, 56);
    m->ch = CH(c, 112);
    m->ch_b = CH(c, 128);
    m->has_b = true;
    m->tgt = F(c, 144, 24);
    break;
  case 0xcb: // call alert: source 24, source SUID 48, target 104
  case 0xdb: // status query: the same
    m->kind = s->opcode == 0xcb ? P25_CALL_ALRT : P25_STS_Q;
    m->src = F(c, s->opcode == 0xcb ? 24 : 104, 24);
    m->tgt = F(c, s->opcode == 0xcb ? 104 : 24, 24);
    suid(c, 48);
    break;
  case 0xcd: // radio unit monitor command: multiplier 38, target 40, source SUID 64, source 120
  case 0xce: // message update: message 24, target 40, source SUID 64, source 120
  case 0xd9: // status update: unit status 24, user status 32, target 40, source SUID 64, source 120
    m->kind = s->opcode == 0xcd ? P25_RAD_MON_CMD : s->opcode == 0xce ? P25_MSG_UPDT : P25_STS_UPDT;
    m->tx_mult = F(c, 38, 2);
    m->message = F(c, 24, 16);
    m->unit_status = F(c, 24, 8);
    m->user_status = F(c, 32, 8);
    m->tgt = F(c, 40, 24);
    suid(c, 64);
    m->src = F(c, 120, 24);
    break;
  case 0xde: // radio unit monitor enhanced command: target 24, source SUID 48; continuation: source 16 (144)
    m->kind = P25_RAD_MON_CMD;
    m->tgt = F(c, 24, 24);
    suid(c, 48);
    m->src = F(c, 144, 24);
    break;
  case 0xe0: // acknowledge response (FNE): service type 26, source SUID 32; continuation: target 40 (168)
    m->kind = P25_ACK_RSP_FNE;
    m->svc_type = F(c, 26, 6);
    suid(c, 32);
    m->tgt = F(c, 168, 24);
    break;
  case 0x71: // authentication demand: target 24, target SUID 48, then the challenge
    m->kind = P25_AUTH_DMAND;
    m->tgt = F(c, 24, 24);
    break;
  default:
    s->mac_kind = P25_MAC_OTHER;
    break;
  }
}

static void decode_struct(struct pdu_bits const *b, struct p25_mac_struct *s, bool multi){
  struct ctx c = { b, s };
  struct p25_msg *m = &s->msg;
  memset(m, 0, sizeof *m);
  if(multi)
    decode_multi(&c);
  else if(s->vendor == P25_MFID_MOTOROLA)
    decode_motorola(&c);
  else if(s->vendor == P25_MFID_HARRIS)
    decode_harris(&c);
  else if(!vendor_partition(s->opcode))
    decode_standard(&c);
  // else another manufacturer's: unknown
  m->family = P25_FAMILY_MAC;
  m->opcode = s->opcode;
  m->mfid = s->vendor;
  m->raw_len = s->raw_len < (int)sizeof m->raw ? s->raw_len : (int)sizeof m->raw;
  memcpy(m->raw, s->raw, m->raw_len);
}

// The PDU header, then PTT and END PTT from their fixed positions, or the walk over the
// structures. algid starts as clear so that only a PTT can say a transmission is encrypted.
// Each structure keeps its raw octets (up to P25_MAC_MAX_RAW, and only those inside the PDU)
// for the log of kinds that are not decoded
int p25_mac_decode(struct p25_mac_pdu *pdu, enum p25_mac_chan chan, uint8_t const *octets, int nbits){
  if(pdu == NULL)
    return -1;
  memset(pdu, 0, sizeof *pdu);
  pdu->chan = chan;
  pdu->algid = P25_ALGID_CLEAR;
  if(octets == NULL || nbits < 8)
    return -1;
  struct pdu_bits b = { octets, nbits };
  pdu->type = get_field(&b, 0, 3);
  pdu->offset_4v = get_field(&b, 3, 3);
  // The protected bit (header bit 0, BBAD-A 2.1 and 4.3): a receiver that cannot undo the
  // protection discards the PDU, so nothing in it (a PTT's encryption, a grant) is ever believed
  pdu->protect = get_field(&b, 7, 1);
  if(pdu->protect)
    return -1;
  if(chan == P25_MAC_LCCH)
    pdu->lcch_nac = get_field(&b, 152, 12);

  switch(pdu->type){
  case P25_MAC_PTT:
    for(int i = 0; i < 9; i++)
      pdu->mi[i] = get_field(&b, 8 + 8 * i, 8);
    pdu->algid = get_field(&b, 80, 8);
    pdu->kid = get_field(&b, 88, 16);
    pdu->src = get_field(&b, 104, 24);
    pdu->group = get_field(&b, 128, 16);
    return 0;
  case P25_MAC_END_PTT:
    pdu->nac = get_field(&b, 12, 12);
    pdu->src = get_field(&b, 104, 24);
    pdu->group = get_field(&b, 128, 16);
    return 0;
  case P25_MAC_SIGNAL:
  case P25_MAC_IDLE:
  case P25_MAC_ACTIVE:
  case P25_MAC_HANGTIME:
    break;
  default:
    return 0;
  }

  // Structures from bit 8 as long as they fit (BBAD-A 2.2.3 sets no count); a NULL INFORMATION
  // opcode ends the list
  int const maxidx = max_index(chan);
  int offset = 8;
  for(int n = 0; n < P25_MAC_MAX_STRUCT && offset + 8 <= maxidx && offset + 8 <= nbits; n++){
    uint8_t const op = get_field(&b, offset, 8);
    if(n > 0 && op == 0x00)
      break;
    struct p25_mac_struct *s = &pdu->s[pdu->nstruct++];
    s->opcode = op;
    s->offset = offset;
    s->len = struct_len(&b, maxidx, offset, op, &s->vendor);
    int k = s->len < P25_MAC_MAX_RAW ? s->len : P25_MAC_MAX_RAW;
    if(k > (nbits - offset) / 8)
      k = (nbits - offset) / 8;
    for(int i = 0; i < k; i++)
      s->raw[i] = get_field(&b, offset + 8 * i, 8);
    s->raw_len = k;
    decode_struct(&b, s, false);
    if(op == 0x00 || s->len <= 0)
      break;
    offset += s->len * 8;
  }
  return pdu->nstruct;
}

// ---- Multi-fragment messages (TIA-102.BBAD-A 3, 4.7, 4.11, 4.12) ----
// A message too long for one PDU (the extended-address LCCH forms, and the authentication demand)
// is sent as a first fragment, one of the opcodes below, filling an LCCH PDU (18 octets), then
// continuation fragments (opcode 0x10) in the following LCCH bursts, in order: there is no
// sequence number. Only the LCCH carries them. Every fragment has its length (its octets in this
// PDU) in the low 6 bits of its second octet; the first fragment's third octet is the data length,
// the message's octets less the first two of each fragment. The data ends with a Multi-Fragment
// CRC-16 over the rest of it. The opcodes and lengths are Table 4's; sdrtrunk
// (MacStructureMultiFragment.java; Dennis Sheirer, GPL-3) reads them the same way, and does not
// check the CRC.

bool p25_mac_multi_frag(uint8_t opcode){
  switch(opcode){
  case 0x71: case 0xc7: case 0xcb: case 0xcd: case 0xce: case 0xcf: case 0xd9: case 0xdb: case 0xde: case 0xe0:
    return true;
  default:
    return false;
  }
}

// The Multi-Fragment CRC-16 (BBAD-A 4.7): the FDMA header block's CRC, generator
// x^16 + x^12 + x^5 + 1, result inverted, over n octets
static uint16_t frag_crc(uint8_t const *p, int n){
  uint16_t crc = 0;
  for(int i = 0; i < 8 * n; i++){
    int const bit = (p[i >> 3] >> (7 - (i & 7))) & 1;
    bool const top = (crc & 0x8000) != 0;
    crc <<= 1;
    if(top != (bit != 0))
      crc ^= 0x1021;
  }
  return crc ^ 0xffff;
}

void p25_mac_frag_reset(struct p25_mac_frag *f){
  if(f != NULL)
    memset(f, 0, sizeof *f);
}

bool p25_mac_frag_add(struct p25_mac_frag *f, struct p25_mac_pdu const *pdu, struct p25_mac_struct *out){
  if(f == NULL || pdu == NULL || out == NULL)
    return false;
  bool done = false, fragment = false;
  for(int i = 0; i < pdu->nstruct; i++){
    struct p25_mac_struct const *s = &pdu->s[i];
    int const n = s->len < s->raw_len ? s->len : s->raw_len; // octets of it the PDU holds
    if(s->vendor == 0 && p25_mac_multi_frag(s->opcode)){
      // A first fragment: whatever was pending is dropped
      p25_mac_frag_reset(f);
      fragment = true;
      if(n < 3 || s->raw[2] < 3 || s->raw[2] + 2 > P25_MAC_FRAG_MAX)
        continue; // too short for a CRC, or a length no buffer here could hold: garbage
      f->active = true;
      f->opcode = s->opcode;
      f->need = s->raw[2];
      memcpy(f->buf, s->raw, n);
      f->len = n;
      f->have = n - 2;
    } else if(s->vendor == 0 && s->opcode == 0x10){
      if(!f->active)
        continue; // a continuation of nothing we have: ignored
      fragment = true;
      if(n < 3 || n != s->len || f->len + n - 2 > P25_MAC_FRAG_MAX){
        p25_mac_frag_reset(f); // a length that cannot be right
        continue;
      }
      memcpy(f->buf + f->len, s->raw + 2, n - 2);
      f->len += n - 2;
      f->have += n - 2;
    } else {
      if(s->mac_kind != P25_MAC_NULL && s->mac_kind != P25_MAC_NULL_AZB)
        p25_mac_frag_reset(f); // any other message ends an assembly
      continue;
    }
    if(f->active && f->have >= f->need){
      // Complete. Its data (from the first fragment's third octet) ends with the CRC: a message
      // put together from the wrong parts, or in the wrong order, fails it
      uint8_t const *d = f->buf + 2;
      int const n = f->need - 2;
      if(frag_crc(d, n) != (d[n] << 8 | d[n + 1])){
        p25_mac_frag_reset(f);
        continue;
      }
      // Parse it as one structure of f->len octets
      memset(out, 0, sizeof *out);
      out->opcode = f->opcode;
      out->len = f->len;
      out->raw_len = f->len < P25_MAC_MAX_RAW ? f->len : P25_MAC_MAX_RAW;
      memcpy(out->raw, f->buf, out->raw_len);
      struct pdu_bits b = { f->buf, f->len * 8 };
      decode_struct(&b, out, true);
      p25_mac_frag_reset(f);
      done = true;
    }
  }
  // The continuations come in the PDUs right after the first fragment: a PDU without one means
  // one was lost (or never sent), and what is pending would join the wrong parts
  if(!fragment && f->active)
    p25_mac_frag_reset(f);
  return done;
}

// A structure with a p25msg kind goes as it is, whatever the kind (P25_UNKNOWN included: the
// trunking logic ignores kinds it does not act on). A multiple grant update becomes one
// GRP_V_CH_GRANT_UPDT (or _EXP, uplink in ch_b) per grant grant() kept: the first always, a
// later one not if it names group 0 or the first grant's group (0x05: any earlier one's)
int p25_mac_trunk_msgs(struct p25_mac_struct const *s, struct p25_msg out[3]){
  if(s == NULL || out == NULL)
    return 0;
  if(s->mac_kind == P25_MAC_MSG){
    out[0] = s->msg;
    return 1;
  }
  if(s->mac_kind != P25_MAC_GRP_V_CH_GRANT_UPDT_MULT && s->mac_kind != P25_MAC_GRP_V_CH_GRANT_UPDT_MULT_EXP)
    return 0;
  bool const exp = s->mac_kind == P25_MAC_GRP_V_CH_GRANT_UPDT_MULT_EXP;
  int n = 0;
  for(int g = 0; g < s->ngrants && n < 3; g++, n++){
    struct p25_msg *m = &out[n];
    memset(m, 0, sizeof *m);
    m->family = P25_FAMILY_MAC;
    m->opcode = s->opcode;
    m->mfid = s->vendor;
    m->kind = exp ? P25_GRP_V_CH_GRANT_UPDT_EXP : P25_GRP_V_CH_GRANT_UPDT;
    m->svc = s->grant[g].svc;   // the multiple forms carry service options; the single implicit one does not
    m->ch = s->grant[g].ch;
    m->ch_b = s->grant[g].ch_ul;
    m->has_b = exp;
    m->group = s->grant[g].group;
  }
  return n;
}

// Names

static char const *const Mac_kind_names[P25_MAC_KIND_COUNT] = {
  [P25_MAC_NULL] = "NULL",
  [P25_MAC_NULL_AZB] = "NULL_AZB",
  [P25_MAC_MULTI_FRAG] = "MULTI_FRAG",
  [P25_MAC_GRP_PAGING] = "GRP_PAGING",
  [P25_MAC_IND_PAGING] = "IND_PAGING",
  [P25_MAC_GRP_V_CH_GRANT_UPDT_MULT] = "GRP_V_CH_GRANT_UPDT_MULT",
  [P25_MAC_GRP_V_CH_GRANT_UPDT_MULT_EXP] = "GRP_V_CH_GRANT_UPDT_MULT_EXP",
  [P25_MAC_PWR_CTL] = "PWR_CTL",
  [P25_MAC_RELEASE] = "MAC_RELEASE",
  [P25_MAC_GRG_V_CH_USR] = "GRG_V_CH_USR",
  [P25_MAC_HARRIS_DATA_GRANT] = "HARRIS_DATA_GRANT",
  [P25_MAC_HARRIS_ALIAS] = "HARRIS_ALIAS",
  [P25_MAC_OTHER] = "OTHER",
};

char const *p25_mac_kind_name(struct p25_mac_struct const *s){
  if(s->mac_kind == P25_MAC_MSG)
    return p25_kind_name(s->msg.kind);
  if(s->mac_kind < 0 || s->mac_kind >= P25_MAC_KIND_COUNT)
    return "?";
  return Mac_kind_names[s->mac_kind];
}

char const *p25_mac_pdu_name(uint8_t type){
  static char const *const names[8] = {
    "signal", "ptt", "end_ptt", "idle", "active", "reserved5", "hangtime", "reserved7"
  };
  return names[type & 7];
}

char const *p25_mac_chan_name(enum p25_mac_chan chan){
  return chan == P25_MAC_FACCH ? "facch" : chan == P25_MAC_LCCH ? "lcch" : "sacch";
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

static void put_hex(struct out *o, char const *key, uint8_t const *p, int len){
  put(o, " %s=", key);
  for(int i = 0; i < len; i++)
    put(o, "%02x", p[i]);
}

// A channel as p25_format() writes it: number, and frequency (and slot) when the plan knows it
static void put_chan(struct out *o, char const *suffix, struct p25_chan ch, struct p25_bandplan const *plan){
  put(o, " ch%s=%u-%u", suffix, ch.iden, ch.num);
  uint64_t dl;
  int slot;
  if(!p25_chan_freq(plan, ch, &dl, NULL, &slot))
    return;
  put(o, " freq%s=%llu.%06llu", suffix, (unsigned long long)(dl / 1000000), (unsigned long long)(dl % 1000000));
  struct p25_band const *b = &plan->band[ch.iden];
  if(b->tdma && b->slots > 1)
    put(o, " slot%s=%d", suffix, slot);
}

// Text that came off the air: printable ASCII, anything else as '?'
static void put_text(struct out *o, char const *key, char const *s){
  put(o, " %s=\"", key);
  for(; *s; s++)
    put(o, "%c", ((uint8_t)*s >= 0x20 && (uint8_t)*s < 0x7f && *s != '"') ? *s : '?');
  put(o, "\"");
}

// A kind p25msg knows: its own formatter writes the fields
static void put_msg(struct out *o, struct p25_mac_struct const *s, struct p25_bandplan const *plan){
  struct p25_msg const *m = &s->msg;
  switch(m->kind){
  case P25_LC_MOT_ALIAS_HDR:
    put(o, " mfid=0x%02x opcode=0x%02x", m->mfid, m->opcode);
    put(o, " tg=%u blocks=%u seq=%u format=%u", m->group, m->block_count, m->sequence, m->alias_format);
    return;
  case P25_LC_MOT_ALIAS_DATA:
    put(o, " mfid=0x%02x opcode=0x%02x", m->mfid, m->opcode);
    put(o, " block=%u seq=%u", m->block, m->sequence);
    put_hex(o, "data", s->raw + 4, s->raw_len > 4 ? s->raw_len - 4 : 0);
    return;
  case P25_MOT_GRG_ADD:
  case P25_MOT_GRG_DEL:
    put(o, " mfid=0x%02x opcode=0x%02x sg=%u tgs=", m->mfid, m->opcode, m->group);
    for(int i = 0; i < s->nmembers; i++)
      put(o, i ? ",%u" : "%u", s->members[i]);
    return;
  case P25_TELE_INT_ANS_REQ:
    put(o, " unit=%u digits=%s", m->tgt, s->digits);
    return;
  case P25_UNKNOWN:
    put(o, " mfid=0x%02x opcode=0x%02x len=%d", m->mfid, m->opcode, s->len);
    put_hex(o, "raw", s->raw, s->raw_len);
    return;
  default:
    break;
  }
  // p25_format() reads a TSBK the same way; drop its "TSBK <kind>"
  struct p25_msg t = *m;
  t.family = P25_FAMILY_TSBK;
  char line[512];
  p25_format(line, sizeof line, &t, plan);
  size_t const skip = 5 + strlen(p25_kind_name(m->kind));
  put(o, "%s", strlen(line) >= skip ? line + skip : "");
  if(m->has_b && (m->kind == P25_GRP_V_CH_GRANT || m->kind == P25_UU_V_CH_GRANT || m->kind == P25_UU_V_CH_GRANT_UPDT
                  || m->kind == P25_TELE_INT_CH_GRANT || m->kind == P25_TELE_INT_CH_GRANT_UPDT
                  || m->kind == P25_MOT_GRG_CH_GRANT))
    put_chan(o, "_ul", m->ch_b, plan);
  if(m->kind == P25_TIME_DATE_ANN)
    put(o, " sec=%u", s->second);
  if(m->kind == P25_HARRIS_GRG_EXENC_CMD && s->nmembers > 1){
    put(o, " tgs=");
    for(int i = 0; i < s->nmembers; i++)
      put(o, i ? ",%u" : "%u", s->members[i]);
  }
  // WACN and system p25_format() has no place for
  if(s->has_suid)
    put(o, " suid=%05x.%03x.%u", m->wacn, m->sysid, s->suid);
  else if(s->opcode == 0xfe)
    put(o, " wacn=0x%05x", m->wacn);
  else if(s->opcode == 0xf3 || s->opcode == 0xe8 || s->opcode == 0x76)
    put(o, " wacn=0x%05x sys=0x%03x", m->wacn, m->sysid);
}

int p25_mac_format(char *buf, size_t size, struct p25_mac_pdu const *pdu, int index,
                   struct p25_bandplan const *plan){
  struct out o = { .p = buf, .left = size, .n = 0 };
  if(size > 0)
    buf[0] = '\0';
  char const *chan = p25_mac_chan_name(pdu->chan);
  if(index < 0 || index >= pdu->nstruct || pdu->type == P25_MAC_PTT || pdu->type == P25_MAC_END_PTT){
    switch(pdu->type){
    case P25_MAC_PTT:
      put(&o, "MAC PTT chan=%s tg=%u src=%u algid=0x%02x kid=0x%04x", chan, pdu->group, pdu->src, pdu->algid, pdu->kid);
      if(pdu->algid != P25_ALGID_CLEAR)
        put(&o, " enc=1");
      put_hex(&o, "mi", pdu->mi, 9);
      break;
    case P25_MAC_END_PTT:
      put(&o, "MAC END_PTT chan=%s tg=%u src=%u nac=0x%03x", chan, pdu->group, pdu->src, pdu->nac);
      break;
    default:
      put(&o, "MAC PDU chan=%s pdu=%s offset=%u structures=%d", chan, p25_mac_pdu_name(pdu->type), pdu->offset_4v,
          pdu->nstruct);
      break;
    }
    if(pdu->chan == P25_MAC_LCCH)
      put(&o, " nac=0x%03x", pdu->lcch_nac);
    return o.n;
  }

  struct p25_mac_struct const *s = &pdu->s[index];
  struct p25_msg const *m = &s->msg;
  put(&o, "MAC %s chan=%s pdu=%s", p25_mac_kind_name(s), chan, p25_mac_pdu_name(pdu->type));
  switch(s->mac_kind){
  case P25_MAC_MSG:
    put_msg(&o, s, plan);
    break;
  case P25_MAC_NULL:
  case P25_MAC_NULL_AZB:
    break;
  case P25_MAC_GRP_V_CH_GRANT_UPDT_MULT:
  case P25_MAC_GRP_V_CH_GRANT_UPDT_MULT_EXP:
    for(int i = 0; i < s->ngrants; i++){
      char sfx[4] = "";
      if(i)
        snprintf(sfx, sizeof sfx, "_%c", 'a' + i);
      put(&o, " tg%s=%u svc%s=0x%02x", sfx, s->grant[i].group, sfx, s->grant[i].svc);
      put_chan(&o, sfx, s->grant[i].ch, plan);
      if(s->mac_kind == P25_MAC_GRP_V_CH_GRANT_UPDT_MULT_EXP){
        char ul[8];
        snprintf(ul, sizeof ul, "%s_ul", sfx);
        put_chan(&o, ul, s->grant[i].ch_ul, plan);
      }
    }
    break;
  case P25_MAC_GRP_PAGING:
  case P25_MAC_IND_PAGING:
    put(&o, " %s=", s->mac_kind == P25_MAC_GRP_PAGING ? "tgs" : "units");
    for(int i = 0; i < s->npaged; i++)
      put(&o, i ? ",%u" : "%u", s->paged[i]);
    break;
  case P25_MAC_PWR_CTL:
    put(&o, " tgt=%u rf_level=%u ber=%u", m->tgt, s->rf_level, s->ber);
    break;
  case P25_MAC_RELEASE:
    put(&o, " tgt=%u forced=%d audio=%d nac=0x%03x", m->tgt, s->forced, s->audio, s->nac);
    break;
  case P25_MAC_GRG_V_CH_USR:
    put(&o, " sg=%u src=%u", m->group, m->src);
    break;
  case P25_MAC_HARRIS_DATA_GRANT:
    put(&o, " mfid=0x%02x opcode=0x%02x tgt=%u", s->vendor, s->opcode, m->tgt);
    if(s->opcode == 0xac)
      put(&o, " src=%u", m->src);
    put_chan(&o, "", m->ch, plan);
    break;
  case P25_MAC_HARRIS_ALIAS:
    put(&o, " mfid=0x%02x opcode=0x%02x", s->vendor, s->opcode);
    put_text(&o, "alias", s->alias);
    break;
  case P25_MAC_MULTI_FRAG:
  case P25_MAC_OTHER:
  default:
    put(&o, " mfid=0x%02x opcode=0x%02x len=%d", s->vendor, s->opcode, s->len);
    put_hex(&o, "raw", s->raw, s->raw_len);
    break;
  }
  if(s->has_nac && s->mac_kind == P25_MAC_MSG)
    put(&o, " nac=0x%03x", s->nac);
  return o.n;
}
