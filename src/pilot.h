// TS-1 pilot edge detector: coherent fold of a BPSK PPS pilot, sub-sample edge
// C port of hf-timestd's BpskEdgeFineStage (core/bpsk_edge_fine_stage.py)
// Pure: no radiod headers, no allocation after pilot_create(), no threads of its own
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
  bool costas_quiet;       // Costas quiet gate held after every batch that fed this fold
  bool locked;             // costas_quiet && snr_db >= snr_floor_db
  char mode[12];           // "bootstrap" | "tracking"
};

struct pilot_state;
struct pilot_state *pilot_create(struct pilot_config const *cfg);
void pilot_destroy(struct pilot_state *p);
void pilot_reset(struct pilot_state *p);   // drop the fold and the Costas gate, keep config and own offset
// Full forget, for a counter that rebases (radiod INPUT_EPOCH change): pilot_reset
// plus the tracking state, own offset and bootstrap history; a fold waiting for
// pilot_finish turns stale and finishes with no estimate
void pilot_forget(struct pilot_state *p);

// Synchronous use (tests, pilot-replay): feed n samples whose first one sits at
// output counter first_index. Returns 1 and fills *out when a fold completes,
// 0 otherwise, -1 on error (including a gap: first_index not equal to the
// previous call's end). After a gap the fold restarts at first_index with
// these samples. Same as pilot_accumulate with pilot_finish run at each fold end
int pilot_process(struct pilot_state *p, float complex const *samples, int n,
                  int64_t first_index, struct pilot_estimate *out);

// Asynchronous use (radiod): two fold buffers. One thread calls
// pilot_accumulate with each block; when a fold completes it waits in the
// pending slot while accumulation continues into the other buffer, and one
// other thread calls pilot_finish (the costly part, ~0.1 s at 96 kHz).
// pilot_accumulate, pilot_reset and pilot_forget belong to the first thread,
// pilot_finish to the second; pilot_pending and pilot_dropped to either.
// A fold that completes while the slot is still busy is dropped and counted
#define PILOT_GAP 1    // first_index jumped: the fold restarted there
#define PILOT_FOLD 2   // a fold completed and now waits for pilot_finish
int pilot_accumulate(struct pilot_state *p, float complex const *samples, int n,
                     int64_t first_index);  // PILOT_* flags, -1 on bad arguments
int pilot_finish(struct pilot_state *p, struct pilot_estimate *out); // 1 = *out filled; 0 = nothing pending, no fit, or stale
bool pilot_pending(struct pilot_state *p);
uint64_t pilot_dropped(struct pilot_state *p);

// What radiod reports for one fold, placed on its input sample counter
struct pilot_report {
  bool valid;
  uint64_t epoch;            // INPUT_EPOCH the fold came from
  int64_t edge_input_sample; // edge on radiod's input counter, raw (nothing subtracted)
  double edge_input_frac;    // [0,1) of an input sample after edge_input_sample
  int64_t edge_utc_ns;       // named second, ns since the GPS epoch (GPS_TIME's scale)
  int64_t host_time_ns;      // the host clock's reading mapped back to the edge
  double sigma_s;            // |split_half_delta| / 2 / output samprate
  double snr_db;
  bool locked;
  int fold_count;            // seconds folded
  double filter_delay_s;     // channel filter group delay, diagnostic only
  struct pilot_estimate est; // battery fields for the listener
};
// Output sample out_offset + *frac_io after the output sample at
// block_input_index, on the input counter; *frac_io returns the input-sample fraction
int64_t pilot_out_to_input(int64_t block_input_index, int64_t out_offset, int decim, double *frac_io);
int64_t pilot_name_second(int64_t edge_host_ns);   // nearest whole second, half up
// Fill *r from estimate *e. base_input: input sample of output counter 0;
// now_ns, now_input: a host clock reading and the input count at that moment
void pilot_make_report(struct pilot_report *r, struct pilot_estimate const *e, uint64_t epoch,
                       int64_t base_input, int decim, int out_samprate, double in_samprate,
                       int64_t now_ns, int64_t now_input, double filter_delay_s);
// Where an input epoch begins: the input sample index of the first sample
// after the loss (the next sample to arrive when the loss was seen)
struct pilot_epoch_mark {
  uint64_t epoch;
  int64_t start;
};
#define PILOT_EPOCH_MARKS 16 // recent marks the front end keeps
// The epoch of the block [s, s+len): the newest marked epoch above current
// whose start is at or before s, else current. Returns 1 when some newer
// mark falls strictly inside the block (the block spans a loss: it keeps the
// epoch of its first sample but must not feed the detector), else 0
int pilot_epoch_for_block(struct pilot_epoch_mark const *marks, int n, uint64_t current,
                          int64_t s, int64_t len, uint64_t *epoch_out);
// False for a block whose input index did not advance past the previous
// block's: radiod's lapped-ring zero block (filter.c execute_filter_output)
bool pilot_block_fresh(bool have_last, int64_t last_index, int64_t index);
// On an epoch other than *epoch_seen: forget everything, invalidate *last,
// take the new epoch, return true. Otherwise false
bool pilot_epoch_check(struct pilot_state *p, struct pilot_report *last, uint64_t *epoch_seen, uint64_t epoch);

#endif
