// Routines for reading and writing formatted text strings to external file attributes
// Should be portable to Linux and Mac OSX, which are gratuitously different
// Copyright 2018-2023, Phil Karn, KA9Q

#ifndef _ATTR_H
#define _ATTR_H 1

int attrscanf(int fd,char const *name,char const *format, ...);
int attrprintf(int fd,char const *attr,char const *format, ...);

#endif
