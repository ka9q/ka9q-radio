#include <stdio.h>
#include <csv.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <sys/types.h>
#include <dirent.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include "database.h"

#define EARTH_RADIUS (6371137) // earth radius

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

static struct data Output_data;
static struct data Input_data;

static inline double sinpi(double x){
  return sin(M_PI * x);
}
static inline double cospi(double x){
  return cos(M_PI * x);
}
static int sort_output_compare(void const *p1, void const *p2);
static int sort_input_compare(void const *p1, void const *p2);
static length_t distance(degree_t const lat, degree_t const longit, repeater_t const * const r);
static void cb1(void *field, size_t size, void *user);
static void cb2(int eol, void *user);
static int scan_filter(const struct dirent *dir);
static int b_out_compare(const void *a, const void *b);
static int b_in_compare(const void *a, const void *b);
static int load_database(struct data *data, char const * const directory, double lat, double longit, enum sort sort);


//#define TEST
#ifdef TEST
static degree_t const Mylat =  32.860455;
static degree_t const Mylongit = -117.188861;

int main(int argc,char *argv[]){
  load_databases(argv[1],Mylat, Mylongit);
  struct data const * const data = &Output_data;
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
    printf("%s ",r->sort == SORT_INPUT ? "input" : "output");
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
#endif

int load_databases(char const * const directory, double lat, double longit){
  load_database(&Output_data, directory, lat, longit,SORT_OUTPUT);
  load_database(&Input_data,  directory, lat, longit,SORT_INPUT);
  return 0;
}

int load_database(struct data *data, char const * const directory, double lat, double longit, enum sort sort){
  if(directory == NULL)
    return -1;

  struct dirent **namelist = NULL;

  int n = scandir(directory, &namelist, scan_filter, NULL);
  assert(namelist != NULL);
  char cwd[PATH_MAX];
  getcwd(cwd,sizeof cwd);
  if(chdir(directory) != 0){
    fprintf(stderr,"Can't open directory %s: %s\n", directory, strerror(errno));
    return -1;
  }
  for(int i = 0; i < n; i++){
    struct dirent *dir = namelist[i];
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
    size_t bytes_read;
    while((bytes_read = fread(buf, sizeof buf[0], 1024, fp)) != 0){
      if(csv_parse(&p, buf, bytes_read, cb1, cb2, data) != bytes_read){
	printf("csv_parse error: %s\n", csv_strerror(csv_error(&p)));
	break;
      }
    }
    csv_fini(&p, cb1, cb2, data);
    fclose(fp); fp = NULL;
    csv_free(&p);
    free(dir); dir = NULL;
    free(data->columns); data->columns = NULL; // No longer needed for this file
  }
  chdir(cwd);
  free(namelist); namelist = NULL;

  // Compute all the distances so we can sort by them
  for(int i=0; i < data->n_repeaters; i++){
    repeater_t * r = &data->repeaters[i];
    r->sort = sort;
    r->distance = distance(lat,longit,r);
  }
  if(sort == SORT_INPUT)
      qsort(data->repeaters, data->n_repeaters, sizeof (repeater_t), sort_input_compare);
    else
      qsort(data->repeaters, data->n_repeaters, sizeof (repeater_t), sort_output_compare);
  return 0;
}

// Find index of first matching repeater in database; will be nearest
repeater_t const *search_database(int freq, int tone){
  repeater_t const output_key = {
    .output = freq,
    .output_tone = tone,
  };
  repeater_t const input_key = {
    .input = freq,
    .input_tone = tone,
  };
  // searches an array of pointers to repeater_t
  repeater_t const *output_entry = bsearch(&output_key, Output_data.repeaters, Output_data.n_repeaters, sizeof(repeater_t), b_out_compare);
  repeater_t const *input_entry = bsearch(&input_key, Input_data.repeaters, Input_data.n_repeaters, sizeof(repeater_t), b_in_compare);
  if(output_entry == NULL)
    return input_entry; // which might be null
  if(input_entry == NULL)
    return output_entry;
  // Backtrack to first matching entry (the closest match)
  for(; output_entry > Output_data.repeaters; output_entry--){
    if(b_out_compare(output_entry,output_entry-1) != 0)
      break;
  }
  for(; input_entry > Input_data.repeaters; input_entry--){
    if(b_in_compare(input_entry,input_entry-1) != 0)
      break;
  }
  return input_entry->distance < output_entry->distance ? input_entry : output_entry;
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
// compare function for repeater list sort
// Sort first by output frequency, then PL tone, then by increasing distance
static int sort_output_compare(void const *p1, void const *p2){
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
  if(r1->output_tone > r2->output_tone)
    return +1;
  if(r1->output_tone < r2->output_tone)
    return -1;
  if(r1->distance > r2->distance)
    return +1;
  if(r1->distance < r2->distance)
    return -1;
  return 0; // unlikely
}
static int sort_input_compare(void const *p1, void const *p2){
  assert(p1 != NULL);
  assert(p2 != NULL);
  if(p1 == NULL || p2 == NULL)
    return +1;
  repeater_t const *r1 = (repeater_t *)p1;
  repeater_t const *r2 = (repeater_t *)p2;
  if(r1 == r2)
    return 0; // can this happen?

  if(r1->input > r2->input)
    return +1;
  if(r1->input < r2->input)
    return -1;
  // If no output tone is listed, assume it's the same as the input tone (if any)
  if(r1->input_tone > r2->input_tone)
    return +1;
  if(r1->input_tone < r2->input_tone)
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
static int b_out_compare(const void *a, const void *b){
  repeater_t const *r1 = (repeater_t *)a;
  repeater_t const *r2 = (repeater_t *)b;
  assert(r1 != NULL && r2 != NULL);
  if(r1->output < r2->output)
    return -1;
  if(r1->output > r2->output)
    return +1;
  //  int const tone1 = r1->output_tone ? r1->output_tone : r1->input_tone;
  //int const tone2 = r2->output_tone ? r2->output_tone : r2->input_tone;
  int const tone1 = r1->output_tone;
  int const tone2 = r2->output_tone;
  if(tone1 < tone2)
    return -1;
  if(tone1 > tone2)
    return +1;
  return 0;
}
static int b_in_compare(const void *a, const void *b){
  repeater_t const *r1 = (repeater_t *)a;
  repeater_t const *r2 = (repeater_t *)b;
  assert(r1 != NULL && r2 != NULL);
  if(r1->input < r2->input)
    return -1;
  if(r1->input > r2->input)
    return +1;
  if(r1->input_tone < r2->input_tone)
    return -1;
  if(r1->input_tone > r2->input_tone)
    return +1;
  return 0;
}

char const *format_freq(char *output, size_t len, int f){
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
char const *format_tone(char *output, size_t len, int f){
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
