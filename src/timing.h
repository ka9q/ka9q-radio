#ifndef _TIMING_H
#define _TIMING_H 1
#include <stdint.h>
#include <time.h>
#include <stdlib.h>

static int64_t const BILLION = 1000000000LL;
static int const MILLION = 1000000;
static int const THOUSAND = 1000;

#define TAI_GPS_OFFSET (19) // TAI is always and forever 19 seconds ahead of GPS
#define GPS_UTC_OFFSET (18) // GPS ahead of utc by 18 seconds - make this a table!
#define TAI_UTC_OFFSET (TAI_GPS_OFFSET+GPS_UTC_OFFSET)
#define UNIX_EPOCH ((time_t)315964800) // GPS epoch on unix time scale

extern char const *Months[12];

char *format_gpstime(char *result,int len,int64_t t);
char *format_gpstime_iso8601(char *result,int len,int64_t t);
char *format_utctime(char *result,int len,int64_t t);
char *format_utctime_iso8601(char *result,int len,int64_t t);
char *ftime(char *result,int size,int64_t t);
void normalize_time(struct timespec *x);
// Operations on timespect structures
// Result = a - b
static inline void time_sub(struct timespec *result,struct timespec const *a, struct timespec const *b){
  result->tv_sec = a->tv_sec - b->tv_sec;
  result->tv_nsec = a->tv_nsec - b->tv_nsec;
  normalize_time(result);
}
// Result = a + b
static inline void time_add(struct timespec *result,struct timespec const *a, struct timespec const *b){
  result->tv_sec = a->tv_sec + b->tv_sec;
  result->tv_nsec = a->tv_nsec + b->tv_nsec;
  normalize_time(result);
}
// Compare two timespec structures, assuming normalized
// a > b: +1
// a < b: -1
// a == b: 0
static inline int time_cmp(struct timespec const *a,struct timespec const *b){
  // Will this long conditional help the optimizer?
  return (a->tv_sec > b->tv_sec) ? 1
    : (a->tv_sec < b->tv_sec) ? -1
    : (a->tv_nsec > b->tv_nsec) ? +1
    : (a->tv_nsec < b->tv_nsec) ? -1
    : 0;
}
// Convert timespec (seconds, nanoseconds) to integer nanoseconds
// Integer nanoseconds overflows past 584.94242 years. That's probably long enough
static inline int64_t ts2ns(struct timespec const *ts){
  return ts->tv_sec * BILLION + ts->tv_nsec;
}
// Convert integer nanosec count to timspec
static inline void ns2ts(struct timespec *ts,int64_t ns){
  lldiv_t r = lldiv(ns,BILLION);
  ts->tv_sec = r.quot;
  ts->tv_nsec = r.rem;
}
// Return time of day as seconds (truncated) from UTC epoch
static inline time_t utc_time_sec(void){
  struct timespec now;
  clock_gettime(CLOCK_REALTIME,&now);
  return (time_t)now.tv_sec;
}
// Same from GPS epoch
static inline time_t gps_time_sec(void){
  return utc_time_sec() - (UNIX_EPOCH - GPS_UTC_OFFSET);
}
// Return time of day as nanosec from UTC epoch
static inline int64_t utc_time_ns(void){
  struct timespec now;
  clock_gettime(CLOCK_REALTIME,&now);
  return ts2ns(&now);
}
// Return time of day as nanosec from GPS epoch
// Note: assumes fixed leap second offset
// Could be better derived direct from a GPS receiver without applying the leap second offset
int64_t gps_time_ns(void);
#endif
