#ifndef _MORSE_H
#define _MORSE_H 1
#include <stdint.h>
#include <wchar.h>
#include <wctype.h>

int encode_morse_char(int16_t *samples,wint_t c);
int init_morse(float const speed,float const pitch,float const level,float const samprate);
#endif
