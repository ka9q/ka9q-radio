// Morse code generation
// Copyright 2022-2023, Phil Karn, KA9Q

#ifndef _MORSE_H
#define _MORSE_H 1
#include <stdint.h>
#include <wchar.h>
#include <wctype.h>

unsigned long encode_morse_char(float *samples,wint_t c);
int init_morse(double const speed,double const pitch,double const level,double const samprate);
#endif
