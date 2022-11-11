// $Id: bandplan.c,v 1.14 2022/04/14 10:50:43 karn Exp $
// Routines for processing the file bandplan.txt
// containing general information about ham radio bandplans, other radio channels, etc
// This information is displayed in the 'Info' window by the 'radio' program
// Copyright 2018, Phil Karn, KA9Q
#define _GNU_SOURCE 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(linux)
#include <bsd/string.h>
#endif
#include <ctype.h>
#include <math.h>
#include <limits.h>

#include "conf.h"
#include "misc.h"
#include "bandplan.h"

char Bandplan_file[] = BANDPLAN; 
#define MAX_BANDPLANS 1000
struct bandplan Bandplans[MAX_BANDPLANS];
int Nbandplans;

// Sort callback function
static int compar(void const *a,void const *b){
  const double f = *(double *)a;
  const struct bandplan *bp = b;

  if(f < bp->lower)
    return -1;
  if(f > bp->upper)
    return 1;
  else
    return 0;
}

static int Bandplan_init;
extern int init_bandplan(void);
static double Cache_freq;
static struct bandplan *Cache_bandplan;


// Look up a given frequency, return pointer to appropriate entry
struct bandplan *lookup_frequency(double f){
  // we get repeatedly called with the same frequency, so cache the last key/entry pair
  if(f == Cache_freq)
    return Cache_bandplan;

  double key;
  key = round(f) / 1.0e6;

  if(!Bandplan_init){
    init_bandplan();
    Bandplan_init = 1;
  }
  struct bandplan *result = bsearch(&key,Bandplans,Nbandplans,sizeof(struct bandplan),compar);
  Cache_freq = f;
  Cache_bandplan = result;
  return result;
}


// Read bandplan.txt, initialize data structures
int init_bandplan(){
  char fname[PATH_MAX];

  if (dist_path(fname,sizeof(fname),Bandplan_file) == -1)
    return -1;

  FILE * const fp = fopen(fname,"r");
  if(fp == NULL)
    return -1;

  char line[160];
  memset(line,0,sizeof(line));
  int i=0;
  for(i=0; i < MAX_BANDPLANS && fgets(line,sizeof(line),fp) != NULL;){
    if(line[0] == ';' || line[0] == '#')
      continue;
    chomp(line);

#if 0
    double lower,upper,bw;
    char description[160];

    char *ptr = NULL;
    lower = strtod(line,&ptr);
    if(ptr == line)
      continue; // no conversion
    while(isspace(*ptr)) // skip space between first two fields
      ptr++;
    char *ptr2 = NULL;
    if(tolower(*ptr) == 'b'){
      // Bandwidth specified
      ptr++; // skip 'b'
      bw = strtod(ptr,&ptr2); // Read bandwidth
      if(ptr == ptr2)
	continue;
      upper = lower + bw/2; // Lower was actually center
      lower -= bw/2;        // center -> lower
    } else {
      upper = strtod(ptr,&ptr2);
      if(ptr == ptr2)
	continue;
    }
    while(isspace(*ptr)) // skip space between first two fields
      ptr++;
#else
    char const *description;
    double center,bw,lower,upper;
    int nchar;
    if(sscanf(line,"%lf b%lf%n",&center,&bw,&nchar) == 2){
      lower = center - bw/2;
      upper = lower + bw;
    } else if(sscanf(line,"%lf %lf%n",&lower,&upper,&nchar) < 2)
      continue; // bad line
    description = line + nchar;
    // Skip leading whitespace on description field
    while(isspace(*description))
      description++;
#endif

    memset(&Bandplans[i],0,sizeof(struct bandplan));
    Bandplans[i].lower = lower;
    Bandplans[i].upper = upper;
    strlcpy(Bandplans[i].description,description,sizeof(Bandplans[i].description));
    i++;
  }
  Nbandplans = i;
#if 0
  fprintf(stderr,"Nbandplans %d\n",Nbandplans);
#endif

  return 0;
}
#if 0
// Standalone test driver program
int main(){
  double f;
  struct bandplan const *bp;

  while(1){
    scanf("%lf",&f);
    bp = lookup_frequency(f);
    if(bp == NULL)
      printf("null\n");
    else
      printf("%lf: %lf - %lf: %s\n",f,bp->lower,bp->upper,bp->description);
  }
}

#endif
