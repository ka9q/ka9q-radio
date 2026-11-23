// Status TLVs for the TS-1 pilot detector: INPUT_EPOCH, ANCHOR_*, PILOT_*
// Used by radio_status.c (encode), decode_status.c (decode) and the roundtrip test
// Copyright 2026, HamSCI
#ifndef _PILOT_STATUS_H
#define _PILOT_STATUS_H 1
#include <stdint.h>
#include <stdbool.h>
#include "status.h"
#include "pilot.h"

// One linear channel's anchor and latest pilot report, as carried in a packet
struct pilot_status {
  uint64_t epoch;            // INPUT_EPOCH: the epoch the anchor (and any pilot group) belongs to
  uint32_t anchor_rtp;
  int64_t anchor_input_sample;
  struct pilot_report report; // report.valid set when the PILOT_* group arrived
};

// Append INPUT_EPOCH and ANCHOR_*; append the PILOT_* group only when
// report->valid && report->epoch == epoch (a report from an older epoch is
// stale: omitted rather than sent beside an anchor it does not match)
void encode_pilot_status(uint8_t **bp,uint64_t epoch,uint32_t anchor_rtp,int64_t anchor_input_sample,
                         struct pilot_report const *report);
// Decode one TLV into *ps. True when type was one of ours (even if unparsable)
bool decode_pilot_status(struct pilot_status *ps,enum status_type type,uint8_t const *cp,int optlen);

#endif
