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

#endif
