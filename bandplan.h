// $Id: bandplan.h,v 1.7 2021/03/06 19:37:38 karn Exp $
// Bandplan routine data structures and definitions
// Copyright 2018, Phil Karn, KA9Q

#ifndef _BANDPLAN_H
#define _BANDPLAN_H 1

// Amateur license classes authorized for each band segment
#define NOVICE_CLASS 1
#define TECHNICIAN_CLASS 2
#define GENERAL_CLASS 4
#define ADVANCED_CLASS 8
#define EXTRA_CLASS 16

// Emission types authorized for each band segment
#define VOICE 1
#define DATA 2
#define IMAGE 4
#define CW 8

struct bandplan {
  double lower;
  double upper;
  char description[160];
};

struct bandplan const *lookup_frequency(double);

#endif
