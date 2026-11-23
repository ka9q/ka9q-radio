// Status TLVs for the TS-1 pilot detector: encode and decode in one place
// so the roundtrip test exercises exactly what radiod sends
// Copyright 2026, HamSCI
#include <string.h>
#include "status.h"
#include "pilot_status.h"

void encode_pilot_status(uint8_t **bp,uint64_t const epoch,uint32_t const anchor_rtp,int64_t const anchor_input_sample,
                         struct pilot_report const * const r){
  encode_int64(bp,INPUT_EPOCH,epoch);
  encode_int32(bp,ANCHOR_RTP,anchor_rtp);
  encode_int64(bp,ANCHOR_INPUT_SAMPLE,(uint64_t)anchor_input_sample);
  if(!r->valid || r->epoch != epoch)
    return; // no report, or one from an earlier epoch than the anchor
  encode_int64(bp,PILOT_EDGE_SAMPLE,(uint64_t)r->edge_input_sample);
  encode_double(bp,PILOT_EDGE_FRAC,r->edge_input_frac);
  encode_int64(bp,PILOT_EDGE_UTC,(uint64_t)r->edge_utc_ns);
  encode_int64(bp,PILOT_HOST_TIME,(uint64_t)r->host_time_ns);
  encode_double(bp,PILOT_EDGE_SIGMA,r->sigma_s);
  encode_double(bp,PILOT_SNR,r->snr_db);
  encode_bool(bp,PILOT_LOCK,r->locked);
  encode_int(bp,PILOT_FOLD_COUNT,r->fold_count);
  encode_double(bp,PILOT_FILTER_DELAY,r->filter_delay_s);
  float const battery[8] = {
    (float)r->est.plateau_amplitude, (float)r->est.fit_rms, (float)r->est.fold_retention,
    (float)r->est.split_half_delta, (float)r->est.peak_prominence, (float)r->est.transition_width,
    (float)r->est.apex_distance, (float)r->est.edge_offset_samples,
  };
  encode_vector(bp,PILOT_BATTERY,battery,8);
}

bool decode_pilot_status(struct pilot_status * const ps,enum status_type const type,uint8_t const * const cp,int const optlen){
  struct pilot_report * const r = &ps->report;
  switch(type){
  case INPUT_EPOCH:
    ps->epoch = decode_int64(cp,optlen);
    break;
  case ANCHOR_RTP:
    ps->anchor_rtp = decode_int32(cp,optlen);
    break;
  case ANCHOR_INPUT_SAMPLE:
    ps->anchor_input_sample = (int64_t)decode_int64(cp,optlen);
    break;
  case PILOT_EDGE_SAMPLE:
    r->edge_input_sample = (int64_t)decode_int64(cp,optlen);
    r->valid = true; // the group's first tag after the anchor
    break;
  case PILOT_EDGE_FRAC:
    r->edge_input_frac = decode_double(cp,optlen);
    break;
  case PILOT_EDGE_UTC:
    r->edge_utc_ns = (int64_t)decode_int64(cp,optlen);
    break;
  case PILOT_HOST_TIME:
    r->host_time_ns = (int64_t)decode_int64(cp,optlen);
    break;
  case PILOT_EDGE_SIGMA:
    r->sigma_s = decode_double(cp,optlen);
    break;
  case PILOT_SNR:
    r->snr_db = decode_double(cp,optlen);
    break;
  case PILOT_LOCK:
    r->locked = decode_bool(cp,optlen);
    break;
  case PILOT_FOLD_COUNT:
    r->fold_count = decode_int(cp,optlen);
    break;
  case PILOT_FILTER_DELAY:
    r->filter_delay_s = decode_double(cp,optlen);
    break;
  case PILOT_BATTERY:
    if(optlen == 8 * (int)sizeof(float)){
      double v[8];
      for(int i=0; i < 8; i++)
        v[i] = decode_float(cp + 4*i,4);
      r->est.plateau_amplitude = v[0]; r->est.fit_rms = v[1]; r->est.fold_retention = v[2];
      r->est.split_half_delta = v[3]; r->est.peak_prominence = v[4]; r->est.transition_width = v[5];
      r->est.apex_distance = v[6]; r->est.edge_offset_samples = v[7];
    }
    break;
  default:
    return false;
  }
  return true;
}
