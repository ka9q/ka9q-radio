#ifndef _DATABASE_H
#define _DATABASE_H
#include <stdbool.h>

typedef double degree_t;  // angle in degrees
typedef double halfrot_t; // angle in half rotations
typedef double length_t;  // length in meters
struct repeater {
  enum sort {
    SORT_INPUT,
    SORT_OUTPUT,
  } sort;
  char *callsign;
  int output; // Hz
  int input;  // Hz
  int input_tone; // deci_hertz
  int output_tone;   // deci hertz
  degree_t lat;
  degree_t longit;
  char *landmark;
  char *city;
  char *county;
  char *state;
  char *country;
  bool fm;
  bool dmr;
  bool dstar;
  bool p25;
  bool fusion;
  length_t distance;
};
typedef struct repeater repeater_t;

enum field_id {
    FIELD_IGNORE,
    FIELD_CALLSIGN,
    FIELD_OUTPUT_FREQ,
    FIELD_INPUT_FREQ,
    FIELD_INPUT_TONE,
    FIELD_OUTPUT_TONE,
    FIELD_LANDMARK,
    FIELD_LATITUDE,
    FIELD_LONGITUDE,
    FIELD_CITY,
    FIELD_COUNTY,
    FIELD_STATE,
    FIELD_COUNTRY,
    FIELD_FM,
    FIELD_DMR,
    FIELD_DSTAR,
    FIELD_P25,
    FIELD_FUSION,
};

struct data {
  enum field_id *columns;
  int field_index;
  int max_columns;
  repeater_t *repeaters;
  int max_repeaters;
  int n_repeaters;
  bool header_read;
};

int load_databases(char const * const directory, double lat, double longit);
repeater_t const *search_database(int freq, int tone);
char const *format_freq(char *output, size_t len, int f);
char const *format_tone(char *output, size_t len, int f);


#endif
