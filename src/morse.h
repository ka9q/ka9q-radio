// Morse code generation
// Copyright 2022-2023, Phil Karn, KA9Q

#ifndef _MORSE_H
#define _MORSE_H 1
#include <stdint.h>
#include <wchar.h>
#include <wctype.h>

int encode_morse_char(float *samples,wint_t c);
int init_morse(float const speed,float const pitch,float const level,float const samprate);
#endif
