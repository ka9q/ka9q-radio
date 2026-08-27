#include <stdio.h>
#include <csv.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <sys/types.h>
#include <dirent.h>
#include <stdlib.h>
#include <assert.h>

#define EARTH_RADIUS (6371137) // earth radius
#define MAX_REPEATERS (3000)
#define MAX_COLUMNS (100)

typedef double degree_t;  // angle in degrees
typedef double halfrot_t; // angle in half rotations
typedef double length_t;  // length in meters

static degree_t const Mylat =  32.860455;
static degree_t const Mylongit = -117.188861;

struct repeater {
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
static struct field {
  char const *name;
  enum field_id id;
} const Fields[] = {
  {"Callsign", FIELD_CALLSIGN},
  {"Frequency (MHz)", FIELD_OUTPUT_FREQ},
  {"Input Frequency (MHz)", FIELD_INPUT_FREQ},
  {"Tone", FIELD_INPUT_TONE}, // note dupe, resolve this
  {"PL Tone", FIELD_INPUT_TONE},
  {"TSQ Tone", FIELD_OUTPUT_TONE},
  {"Landmark", FIELD_LANDMARK},
  {"City", FIELD_CITY},
  {"County", FIELD_COUNTY},
  {"State", FIELD_STATE},
  {"Country", FIELD_COUNTRY},
  {"Latitude", FIELD_LATITUDE},
  {"Longitude", FIELD_LONGITUDE},
  {"FM (analog)", FIELD_FM},
  {"DMR", FIELD_DMR},
  {"D-STAR Node", FIELD_DSTAR},
  {"P25", FIELD_P25},
  {"System Fusion", FIELD_FUSION},
  {NULL, -1},
};

static struct data {
  enum field_id *columns;
  int field_index;
  int max_columns;
  repeater_t *repeaters;
  int max_repeaters;
  int n_repeaters;
  bool header_read;
} Data;

static inline double sinpi(double x){
  return sin(M_PI * x);
}
static inline double cospi(double x){
  return cos(M_PI * x);
}
static int sort_compare(void const *p1, void const *p2);
static int compar(struct dirent const **p1, struct dirent const **p2);
static length_t distance(degree_t const lat, degree_t const longit, repeater_t const * const r);
static void cb1(void *field, size_t size, void *user);
static void cb2(int eol, void *user);
static char const *format_freq(char *output, size_t len, int f);
static char const *format_tone(char *output, size_t len, int f);
static int scan_filter(const struct dirent *dir);
static int bcompare(const void *a, const void *b);

int load_database(char const *directory,double lat, double longit);
repeater_t const *search_database(int freq, int tone);


int main(int argc,char *argv[]){
  load_database(argv[1],Mylat, Mylongit);
  struct data const * const data = &Data;
  printf("%d repeaters\n",data->n_repeaters);

  while(true){
    double dfreq, dtone;
    printf("enter freq: ");
    scanf("%lf", &dfreq);
    printf("enter tone: ");
    scanf("%lf", &dtone);
    int freq = lround(dfreq * 1e6);
    int tone = lround(dtone * 10.0);
    repeater_t const *r = search_database(freq,tone);
    if(r == NULL){
      printf("not found\n");
      continue;
    }
    if(r->callsign && strlen(r->callsign) > 0)
      printf("%s,",r->callsign);
    else
      printf("unknown,");
    char buffer[128];
    format_freq(buffer, sizeof buffer, r->output);
    if(strlen(buffer) > 0)
      printf(" %s",buffer);
    if(r->output_tone > 0){
      format_tone(buffer, sizeof buffer, r->output_tone);
      printf(" %s",buffer);
    } else if(r->input_tone > 0){
      format_tone(buffer, sizeof buffer, r->input_tone);
      printf(" %s",buffer);
    }
    if(r->landmark && strlen(r->landmark) > 0)
      printf(" %s", r->landmark);
    if(r->city && strlen(r->city) > 0)
       printf(" %s", r->city);
    if(r->county && strlen(r->county) > 0)
      printf(" (%s)", r->county);
    if(r->state && strlen(r->state) > 0)
      printf(" %s", r->state);
    if(r->distance > 0)
      printf("; %.1lf km",0.001 * r->distance);
    printf("\n");

  }
  return 0;
}


int load_database(char const * const directory, double lat, double longit){
  if(directory == NULL)
    return -1;
  struct data *data = &Data;

  struct dirent **namelist = NULL;

  int n = scandir(directory, &namelist, scan_filter, compar);
  assert(namelist != NULL);
  for(int i = 0; i < n; i++){
    struct dirent const * const dir = namelist[i];
    if(dir == NULL)
      continue; // end of list?

    FILE *fp = fopen(dir->d_name,"r");
    if(fp == NULL){
      fprintf(stderr,"Can't read %s\n",dir->d_name);
      continue;
    }
    struct csv_parser p;
    csv_init(&p, CSV_APPEND_NULL);
    char buf[1024];
    data->header_read = false;
    int bytes_read;
    while((bytes_read = fread(buf, sizeof buf[0], 1024, fp)) > 0){
      if(csv_parse(&p, buf, bytes_read, cb1, cb2, data) != bytes_read){
	printf("csv_parse error: %s\n", csv_strerror(csv_error(&p)));
	break;
      }
    }
    csv_fini(&p, cb1, cb2, data);
    fclose(fp); fp = NULL;
    csv_free(&p);
    free(data->columns); data->columns = NULL; // No longer needed for this file
  }
  // Compute all the distances so we can sort by them
  for(int i=0; i < data->n_repeaters; i++){
    repeater_t * const r = &data->repeaters[i];
    r->distance = distance(lat,longit,r);
  }
  qsort(data->repeaters, data->n_repeaters, sizeof (repeater_t), sort_compare);
  return 0;
}

// Find index of first matching repeater in database; will be nearest
repeater_t const *search_database(int freq, int tone){
  repeater_t const key = {
    .output = freq,
    .output_tone = tone,
  };
  // searches an array of pointers to repeater_t
  repeater_t const *entry = bsearch(&key, Data.repeaters, Data.n_repeaters, sizeof(repeater_t), bcompare);
  if(entry == NULL)
    return NULL;
  // Backtrack to first matching entry (the closest match)
  for(; entry > Data.repeaters; entry--){
    if(bcompare(entry,entry-1) != 0)
      return entry;
  }
  return entry;
}

// Callback for scandir() in load_database()
// Ignore files that don't end in .csv
static int scan_filter(const struct dirent *dir){
  if(dir->d_type != DT_REG)
    return 0;
  char const *suffix = ".csv";
  char const *cp = strstr(dir->d_name, suffix);
  if(cp == NULL)
    return 0; // ignore unless .csv
  if(strlen(cp) != strlen(suffix))
    return 0; // is at the end of the file name
  return 1;
}
// First callback for csv_parse() and csv_fini(), called on each field in a .csv
// Bulk of the work is done here
// Allocate room for the column tag table and the repeater table, populate both
// Strings are also dynamically allocated to avoid arbitrary limits
// Fortunately the database is continually used so we don't have to free all this
static void cb1(void * const field, size_t const size, void * const user){
  assert(field != NULL);
  assert(user != NULL);
  if(field == NULL || user == NULL)
    return;
  struct data * const data = (struct data *)user;

  if(data->columns == NULL || data->field_index >= data->max_columns){
    data->max_columns += 32;
    data->columns = realloc(data->columns,data->max_columns * sizeof *data->columns);
    assert(data->columns != NULL);
  }
  if(!data->header_read){
    // Populate list of field headers
    if(size != 0 && field != NULL){
      // Look up column header, convert to enum index
      for(int i=0; Fields[i].name != NULL; i++){
	if(strncasecmp(field, Fields[i].name, size) == 0){
	  data->columns[data->field_index] = Fields[i].id;
	  break;
	}
      }
    }
    data->field_index++; // always increment, even if size == 0
    return;
  }
  // Populate the repeater table
  if(data->repeaters == NULL || data->n_repeaters >= data->max_repeaters){
    data->max_repeaters += 256;
    data->repeaters = realloc(data->repeaters, data->max_repeaters * sizeof(repeater_t));
    assert(data->repeaters != NULL);
  }
  repeater_t * const r = &data->repeaters[data->n_repeaters];
  switch(data->columns[data->field_index]){
  case FIELD_CALLSIGN:
    r->callsign = strdup(field);
    break;
  case FIELD_OUTPUT_FREQ:
    {
      double const f = strtod(field,NULL);
      r->output = lrint(f * 1.e6);
    }
    break;
  case FIELD_INPUT_FREQ:
    {
      double const f = strtod(field,NULL);
      r->input = lrint(f * 1.e6);
    }
    break;
  case FIELD_OUTPUT_TONE:
    {
      if(strcasecmp(field,"CSQ") == 0){
	r->output_tone = 0; // happens anyway
	break;
      }
      double const f = strtod(field,NULL);
      r->output_tone = lrint(f * 10.);
    }
    break;
  case FIELD_INPUT_TONE:
    {
      if(strcasecmp(field,"CSQ") == 0){
	r->input_tone = 0; // happens anyway
	break;
      }
      double const f = strtod(field,NULL);
      r->input_tone = lrint(f * 10.);
    }
    break;
  case FIELD_LANDMARK:
    r->landmark = strdup(field);
    break;
  case FIELD_CITY:
    r->city = strdup(field);
    break;
  case FIELD_COUNTY:
    r->county = strdup(field);
    break;
  case FIELD_STATE:
    r->state = strdup(field);
    break;
  case FIELD_COUNTRY:
    r->country = strdup(field);
    break;
  case FIELD_LATITUDE:
    r->lat = strtod(field,NULL);
    break;
  case FIELD_LONGITUDE:
    r->longit = strtod(field,NULL);
    break;
  case FIELD_FM:
    r->fm = (strcasecmp(field,"Yes") == 0);
    break;
  case FIELD_DMR:
    r->dmr = (strcasecmp(field,"Yes") == 0);
    break;
  case FIELD_DSTAR:
    r->dstar = (strcasecmp(field,"Yes") == 0);
    break;
  case FIELD_P25:
    r->p25 = (strcasecmp(field,"Yes") == 0);
    break;
  case FIELD_FUSION:
    r->fusion = (strcasecmp(field,"Yes") == 0);
    break;
  default:
    break;
  }
  data->field_index++;
}
// Second callback for the csv_ functions, called on end of record character
// Doesn't have to do much, just mark when the first (header) line has been read
// and increment the repeater count after that
static void cb2(int eol, void *user){
  (void)eol; // don't care what the end of record character is
  assert(user != NULL);
  if(user == NULL)
    return;
  struct data * const data = (struct data *)user;

  data->field_index = 0;
  if(!data->header_read){
    data->header_read = true;
    return;
  }
  data->n_repeaters++; // the other callback will realloc if needed
}

// Distance between repeaters a and b
static length_t distance(degree_t const lat, degree_t const longit, repeater_t const * const r){
  // Angles are converted to half rotations, use sinpi() and cospi()
  halfrot_t const delta_phi = (r->lat - lat) / 180.;
  halfrot_t const delta_lambda = (r->longit - longit) / 180.;
  double a = sinpi(delta_phi/2);
  double const b = sinpi(delta_lambda/2);
  a = a*a + cospi(lat/180.) * cospi(r->lat/180.) * b * b;
  a = a > 1 ? 1 : a < 0 ? 0 : a; // clip in case of rounding
  length_t const dist = 2 * EARTH_RADIUS * atan2(sqrt(a), sqrt(1-a));
#if 0 // use later for something
  halfrot_t bearing = (1./M_PI) * atan2(sinpi(delta_lambda) * cospi(r->lat),
	 cospi(lat) * sin(r->lat) - sinpi(lat) * cospi(r->lat) * cospi(delta_lambda));
#endif
  return dist;
}
// compare function for directory sort (scandir needs it)
static int compar(struct dirent const **d, struct dirent const **c){
  return strcmp((*d)->d_name, (*c)->d_name);
}

// compare function for repeater list sort
// Sort first by output frequency, then PL tone, then by increasing distance
static int sort_compare(void const *p1, void const *p2){
  assert(p1 != NULL);
  assert(p2 != NULL);
  if(p1 == NULL || p2 == NULL)
    return +1;
  repeater_t const *r1 = (repeater_t *)p1;
  repeater_t const *r2 = (repeater_t *)p2;
  if(r1 == r2)
    return 0; // can this happen?

  if(r1->output > r2->output)
    return +1;
  if(r1->output < r2->output)
    return -1;
  // If no output tone is listed, assume it's the same as the input tone (if any)
  int tone1 = r1->output_tone ? r1->output_tone : r1->input_tone;
  int tone2 = r2->output_tone ? r2->output_tone : r2->input_tone;
  if(tone1 > tone2)
    return +1;
  if(tone1 < tone2)
    return -1;
  if(r1->distance > r2->distance)
    return +1;
  if(r1->distance < r2->distance)
    return -1;
  return 0; // unlikely
}
// callback for bsearch()
// Does NOT look at distance, that's already sorted in the database
// We'll use it to find the first (closest) matching entry
static int bcompare(const void *a, const void *b){
  repeater_t const *r1 = (repeater_t *)a;
  repeater_t const *r2 = (repeater_t *)b;
  assert(r1 != NULL && r2 != NULL);
  if(r1->output < r2->output)
    return -1;
  if(r1->output > r2->output)
    return +1;
  int const tone1 = r1->output_tone ? r1->output_tone : r1->input_tone;
  int const tone2 = r2->output_tone ? r2->output_tone : r2->input_tone;
  if(tone1 < tone2)
    return -1;
  if(tone1 > tone2)
    return +1;
  return 0;
}

static char const *format_freq(char *output, size_t len, int f){
  if(output == NULL)
    return NULL;
  double fn = 0.000001 * f;
  if(f % 10000 == 0)
    snprintf(output, len, "%.2lf MHz", fn);
  else if(f % 1000 == 0)
    snprintf(output, len, "%.3lf MHz", fn);
  else if(f % 100 == 0)
    snprintf(output, len, "%.4lf MHz", fn);
  else if(f % 10 == 0)
    snprintf(output, len, "%.5lf MHz", fn);
  else
    snprintf(output, len, "%.6lf MHz", fn); // just print hertz
  return output;
}
static char const *format_tone(char *output, size_t len, int f){
  if(output == NULL)
    return NULL;

  if(f == 0)
    *output = '\0'; // don't show anything
  else if(f % 10 == 0)
    snprintf(output, len, "%d Hz",f / 10);
  else
    snprintf(output, len, "%.1lf Hz", 0.1 * f);
  return output;
}
