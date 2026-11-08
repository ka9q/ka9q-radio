// TS-1 pilot edge detector: coherent fold of a BPSK PPS pilot, sub-sample edge
// C port of hf-timestd's BpskEdgeFineStage (core/bpsk_edge_fine_stage.py)
// Pure: no radiod headers, no allocation after pilot_create()
#ifndef _PILOT_H
#define _PILOT_H 1
#include <stdint.h>
#include <stdbool.h>
#include <complex.h>

struct pilot_config {
  int samprate;            // output samples/sec; one fold period
  int fold_seconds;        // seconds coherently folded per estimate
  double search_window_ms; // half-width of the zero-crossing search
  double snr_floor_db;     // locked needs snr_db at or above this
};

struct pilot_estimate {
  int64_t edge_index;      // output-sample counter of the sample at/before the edge
  double edge_frac;        // [0,1) fraction of a sample after edge_index
  double edge_offset_samples; // position within the second, as Python's edge_offset_samples
  int n_seconds;
  double plateau_amplitude, fit_rms, fold_retention, split_half_delta,
         peak_prominence, transition_width, apex_distance, snr_db;
  bool costas_quiet;       // filled by Task 4; true until then
  bool locked;             // costas_quiet && snr_db >= snr_floor_db
  char mode[12];           // "bootstrap" | "tracking"
};

struct pilot_state;
struct pilot_state *pilot_create(struct pilot_config const *cfg);
void pilot_destroy(struct pilot_state *p);
void pilot_reset(struct pilot_state *p);   // drop the fold, keep config
// Feed n samples whose first one sits at output counter first_index.
// Returns 1 and fills *out when a fold completes, 0 otherwise, -1 on error
// (including a gap: first_index not equal to the previous call's end).
// After a gap the fold restarts at first_index with these samples.
int pilot_process(struct pilot_state *p, float complex const *samples, int n,
                  int64_t first_index, struct pilot_estimate *out);

#endif
