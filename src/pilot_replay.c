// pilot-replay FILE.cf32 RATE [FOLD_SECONDS]
// Feed a little-endian interleaved float32 I/Q capture through the pilot detector
// in 1740-sample batches (first_index = file sample index), streaming, never
// holding the file; print one JSON line per estimate with hf-timestd's
// FineEdgeEstimate key names (make_golden.py), plus the C-only fields
// Copyright 2026, HamSCI
#include <complex.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sysexits.h>
#include "pilot.h"

#define BATCH 1740

static void num(char const *key,double v,int last){
  if(isfinite(v))
    printf("\"%s\": %.17g%s",key,v,last ? "" : ", ");
  else
    printf("\"%s\": null%s",key,last ? "" : ", "); // NaN is not JSON
}

int main(int argc,char *argv[]){
  if(argc < 3){
    fprintf(stderr,"Usage: %s FILE.cf32 RATE [FOLD_SECONDS]\n",argv[0]);
    exit(EX_USAGE);
  }
  struct pilot_config cfg = {
    .samprate = atoi(argv[2]),
    .fold_seconds = argc > 3 ? atoi(argv[3]) : 30,
    .search_window_ms = 6.0,
    .snr_floor_db = 20.0,
  };
  FILE *fp = fopen(argv[1],"rb");
  if(fp == NULL){
    perror(argv[1]);
    exit(1);
  }
  struct pilot_state *p = pilot_create(&cfg);
  if(p == NULL){
    fprintf(stderr,"bad config: rate %d fold %d\n",cfg.samprate,cfg.fold_seconds);
    exit(1);
  }
  static float complex buf[BATCH]; // float complex = two little-endian float32 on our targets
  int64_t index = 0;
  size_t n;
  while((n = fread(buf,sizeof(buf[0]),BATCH,fp)) > 0){
    struct pilot_estimate e;
    int const r = pilot_process(p,buf,(int)n,index,&e);
    if(r < 0){
      fprintf(stderr,"pilot_process error at index %lld\n",(long long)index);
      exit(1);
    }
    if(r == 1){
      // Python: edge_rtp = round(registration + c_edge), edge_subsample = remainder
      double const edge = (double)e.edge_index + e.edge_frac;
      double const rounded = nearbyint(edge);
      printf("{\"edge_rtp\": %lld, ",(long long)rounded);
      num("edge_subsample",edge - rounded,0);
      num("edge_offset_samples",e.edge_offset_samples,0);
      printf("\"n_seconds_folded\": %d, ",e.n_seconds);
      num("plateau_amplitude",e.plateau_amplitude,0);
      num("fit_rms",e.fit_rms,0);
      num("fold_retention",e.fold_retention,0);
      num("split_half_delta_samples",e.split_half_delta,0);
      num("peak_prominence",e.peak_prominence,0);
      num("transition_width_samples",e.transition_width,0);
      num("apex_distance_samples",e.apex_distance,0);
      printf("\"edge_index\": %lld, ",(long long)e.edge_index);
      num("edge_frac",e.edge_frac,0);
      num("snr_db",e.snr_db,0);
      printf("\"costas_quiet\": %s, \"locked\": %s, \"mode\": \"%s\"}\n",
             e.costas_quiet ? "true" : "false",e.locked ? "true" : "false",e.mode);
      fflush(stdout);
    }
    index += n;
  }
  fclose(fp);
  pilot_destroy(p);
  exit(0);
}
