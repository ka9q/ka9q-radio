// radiod glue for the TS-1 pilot detector (pilot.c): input counter, epoch, naming the second
// Copyright 2026, HamSCI
#ifndef _PILOT_CHAN_H
#define _PILOT_CHAN_H 1
#include "radio.h"

// Called by demod_linear once per block, right after downconvert() returns a
// new block and before the demod touches chan->baseband. Records chan->anchor
// for every linear channel; with pilot.enable, also feeds the detector and
// collects finished folds into chan->pilot.last
void pilot_on_block(chan_t *chan);
// Called by demod_linear as it exits: stops the worker, frees the detector
void pilot_chan_stop(chan_t *chan);

// Front-end drivers call this where samples go missing: a new input epoch
// begins at input sample `start` (the next sample to arrive, i.e. the
// frontend->samples count just after the loss). seed != 0 sets the epoch to
// seed (start-up) instead of adding 1. Writers are rare and spin briefly on
// epoch_lock; demod threads read the marks lock-free. A mark's slot is first
// invalidated (epoch 0), then start, then epoch is written, so a reader that
// sees the same nonzero epoch before and after reading start has a consistent pair
static inline void input_epoch_begin(struct frontend * const frontend,uint64_t const seed,int64_t const start){
  while(atomic_flag_test_and_set_explicit(&frontend->epoch_lock,memory_order_acquire))
    ;
  uint64_t e = seed != 0 ? seed : atomic_load(&frontend->input_epoch) + 1;
  if(e == 0)
    e = 1;  // 0 marks a slot being rewritten
  int const slot = (int)(e % PILOT_EPOCH_MARKS);
  atomic_store(&frontend->epoch_marks[slot].epoch,0);
  atomic_store(&frontend->epoch_marks[slot].start,start);
  atomic_store(&frontend->epoch_marks[slot].epoch,e);
  atomic_store(&frontend->input_epoch,e);
  atomic_flag_clear_explicit(&frontend->epoch_lock,memory_order_release);
}

#endif
