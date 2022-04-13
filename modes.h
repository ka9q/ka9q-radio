#ifndef _MODES_H
#define _MODES_H 1

#include <stdbool.h>

enum demod_type {
  LINEAR_DEMOD = 0,     // Linear demodulation, i.e., everything else: SSB, CW, DSB, CAM, IQ
  FM_DEMOD,             // Frequency demodulation
  WFM_DEMOD,            // wideband frequency modulation (broadcast)
};

struct demodtab {
  enum demod_type type;
  char name[16];
};

extern struct demodtab Demodtab[];
extern int Ndemod;

char const *demod_name_from_type(enum demod_type type);
int demod_type_from_name(char const *name);

#endif
