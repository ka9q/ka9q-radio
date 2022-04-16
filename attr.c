// $Id: attr.c,v 1.6 2022/04/15 16:14:27 karn Exp $
// Low-level extended file attribute routines
// These are in a separate file mainly because they are so OS-dependent. And gratuitously so.
// Copyright 29 July 2017 Phil Karn, KA9Q

#define _GNU_SOURCE 1
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <alloca.h>
#include <string.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include "attr.h"

// Look for external attribute "name" on an open file and perform scanf on its value
int attrscanf(int fd,char const *name,char const *format, ...){
  int attrlen;
  char *value = NULL;

#ifdef __linux__   // Grrrrr.....
  char *fullname = NULL;
  if(asprintf(&fullname,"user.%s",name) >= 0){
    if((attrlen = fgetxattr(fd,fullname,NULL,0)) >= 0){ // Length of attribute value
      value = alloca(attrlen+1);
      fgetxattr(fd,fullname,value,attrlen);
      value[attrlen] = '\0';
    }
    free(fullname); fullname = NULL;
  }
#else // mainly OSX, probably BSD
  if((attrlen = fgetxattr(fd,name,NULL,0,0,0)) >= 0){
    value = alloca(attrlen+1);
    fgetxattr(fd,name,value,attrlen,0,0);
    value[attrlen] = '\0';
  }
#endif
  int r = -1;

  if(value != NULL){
    va_list ap;
    va_start(ap,format);
    r = vsscanf(value,format,ap);
    va_end(ap);
    // no need to free value, it's on the stack
  }
  return r;
}
// Format an extended attribute and attach it to an open file
int attrprintf(int fd,char const *attr,char const *format, ...){
  va_list ap;
  va_start(ap,format);

  int r = -1;
  char *args = NULL;
  int argslen;
  if((argslen = vasprintf(&args,format,ap)) >= 0){
#ifdef __linux__  // Grrrrrrr....
    char *prefix = NULL;
    if(asprintf(&prefix,"user.%s",attr) >= 0){
      r = fsetxattr(fd,prefix,args,argslen,0);
      free(prefix); prefix = NULL;
    }
#else
    r = fsetxattr(fd,attr,args,argslen,0,0);
#endif
    free(args); args = NULL;
  }
  va_end(ap);
  return r;
}

  
