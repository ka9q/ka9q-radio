// Time representation and conversions for ka9q-radio
// Leap second warning removed -- BIPM has finally abolished leap seconds. It's about freaking time!
// https://www.bipm.org/en/cgpm-2026/resolution-5
// Copyright 2026, Phil Karn, KA9Q

#include <stdatomic.h>
#include <stdint.h>
#include <time.h>
#include <stdio.h>
#include <string.h>
#ifdef __linux__
#include <sys/timex.h>
#endif
#include <assert.h>

#include "timing.h"

extern int Verbose;

int64_t gps_time_ns(void){
  struct timespec ts = {0};
#ifdef CLOCK_TAI
  // Try CLOCK_TAI and also query TAI-UTC offset
  struct timex tx = {0};
  if (clock_gettime(CLOCK_TAI, &ts) == 0 && adjtimex(&tx) != -1 && tx.tai >= 10) {
    /* Strictly speaking CLOCK_TAI isn't actually TAI, which is defined from the epoch of 1 Jan 1958.
       CLOCK_TAI counts seconds from the fictitious UNIX/POSIX epoch of 1 Jan 1970 00:00:00 UTC. (Fictitious because leap seconds
       didn't start until 1972, and POSIX doesn't count them anyway.)
       Since the system time is kept in pseudo-UTC, CLOCK_TAI returns system time plus the now permanent TAI-UTC offset (+37 sec as of Jan 1, 2017)
       TAI is always 19 sec ahead of GPS
       The GPS epoch (Sun 6 Jan 1980 00:00:00 UTC) was 3657 days after the UNIX epoch of Thu 1 Jan 1970 00:00:00 UTC), ignoring the 9
       leap seconds between those two dates (because POSIX does)
    */
    ts.tv_sec -= UNIX_EPOCH;
    ts.tv_sec -= TAI_GPS_OFFSET;
    return ts.tv_sec * BILLION + ts.tv_nsec;
  }
#endif
  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_sec -= UNIX_EPOCH;
  ts.tv_sec += GPS_UTC_OFFSET;
  return ts.tv_sec * BILLION + ts.tv_nsec;
}

void normalize_time(struct timespec *x){
  // Most common cases first
  if(x->tv_nsec < 0){
    x->tv_nsec += BILLION;
    x->tv_sec--;
  } else if(x->tv_nsec >= BILLION){
    x->tv_nsec -= BILLION;
    x->tv_sec++;
  } else
    return;

  // Unlikely to get here
  if(x->tv_nsec < 0 || x->tv_nsec >= BILLION){
    lldiv_t f = lldiv(x->tv_nsec,BILLION);
    x->tv_sec += f.quot;
    x->tv_nsec = f.rem;
    if(x->tv_nsec < 0){ // Sign of remainder for negative numerator is not clearly defined
      x->tv_nsec += BILLION;
      x->tv_sec--;
    }
  }
}
char const *Days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
char const *Months[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec" };

// Format, as UTC, a time measured in nanoseconds from the GPS epoch
char *format_gpstime(char *result,int len,int64_t t){
  return format_utctime(result,len,t + BILLION * (UNIX_EPOCH - GPS_UTC_OFFSET));
}
char *format_gpstime_iso8601(char *result,int len,int64_t t){
  return format_utctime_iso8601(result,len,t + BILLION * (UNIX_EPOCH - GPS_UTC_OFFSET));
}
// Format, as UTC, a time measured in nanoseconds from the UNIX epoch
char *format_utctime(char *result,int len,int64_t t){
  time_t utime = t / BILLION;
  int t_usec = (int)((t % BILLION) / 1000); // ns to us

  if(t_usec < 0){
    t_usec += 1000000;
    utime -= 1;
  }
  struct tm tm = {0};
  gmtime_r(&utime,&tm);
  // Mon Feb 26 2018 14:40:08.123456 UTC
  snprintf(result,len,"%s %02d %s %4d %02d:%02d:%02d.%06d UTC",
	   Days[tm.tm_wday],
	   tm.tm_mday,
	   Months[tm.tm_mon],
	   tm.tm_year+1900,
	   tm.tm_hour,
	   tm.tm_min,
	   tm.tm_sec,
	   t_usec);

  return result;
}
// Format a time in ns since the Unix epoch into ISO 8601, eg, 2018-02-26T14:40:08.123456Z
char *format_utctime_iso8601(char *result,int len,int64_t t){
  time_t utime = t / BILLION;
  int t_usec = (int)((t % BILLION) / 1000);

  if(t_usec < 0){
    t_usec += 1000000;
    utime -= 1;
  }
  struct tm tm = {0};
  gmtime_r(&utime,&tm);
  snprintf(result,len,"%04d-%02d-%02dT%02d:%02d:%02d.%06dZ",
	   tm.tm_year+1900,
	   tm.tm_mon+1,
	   tm.tm_mday,
	   tm.tm_hour,
	   tm.tm_min,
	   tm.tm_sec,
	   t_usec);

  return result;
}

// Format a seconds count into hh:mm:ss
char *ftime(char * result,int size,int64_t t){
  // Init to blanks
  memset(result,0,size);
  char *cp = result;
  // Emit sign
  if(t < 0){
    *cp = '-';
    t = -t; // absolute value
  } else
    *cp = ' ';

  cp++;
  size--;

  int64_t const hr = t / 3600; // Hours is potentially unlimited
  t -= 3600 * hr;

  // Show hours and the hour:minute colon only if hr > 0
  int r = 0;
  if(hr > 0)
    r = snprintf(cp,size,"%lld:",(long long)hr);

  if(r < 0)
    return NULL;
  cp += r;
  size -= r;

  int64_t const mn = t / 60; // minutes is limited to 0-59
  t -= mn * 60;
  assert(mn < 60);
  assert(t < 60);

  r = 0;
  if(hr > 0)
    // hours field is present, show minutes with leading zero
    r = snprintf(cp,size,"%02lld:",(long long)mn);
  else if(mn > 0)
    // Hours zero, show minute without leading 0
    r = snprintf(cp,size,"%lld:",(long long)mn);

  if(r < 0)
    return NULL;
  cp += r;
  size -= r;


  r = 0;
  if(hr > 0 || mn > 0)
  // Hours or minutes are nonzero, show seconds with leading 0
    r = snprintf(cp,size,"%02lld",(long long)t);
  else if(t > 0)
    r = snprintf(cp,size,"%lld",(long long)t);

  if(r < 0)
    return NULL;

  return result;
}

