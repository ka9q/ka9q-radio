// Low-level extended file attribute routines
// These are in a separate file mainly because they are so OS-dependent. And gratuitously so.
// 29 July 2017 Phil Karn
// Copyright 2017-2023  Phil Karn, KA9Q

#define _GNU_SOURCE 1
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
/* Generated feature flags */
#include "ka9q_config.h"

/* alloca() lives in <stdlib.h> on BSD/macOS; Linux also has <alloca.h> */
#include <stdlib.h>
#if HAVE_ALLOCA_H
#  include <alloca.h>
#endif

#include <string.h>
#include <assert.h>
#include <sys/types.h>

#include "compat_xattr.h"
#include <pthread.h>   // for pthread_mutexattr_* if not already included
#include "misc.h"
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
    FREE(fullname);
  }
#else // mainly OSX, probably BSD
  if((attrlen = fgetxattr(fd,name,NULL,0)) >= 0){
    value = alloca(attrlen+1);
    fgetxattr(fd,name,value,attrlen);
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
      FREE(prefix);
    }
#else
    r = fsetxattr(fd,attr,args,argslen,0);
#endif
    FREE(args);
  }
  va_end(ap);
  return r;
}

  
