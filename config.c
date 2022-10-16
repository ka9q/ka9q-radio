// $Id: config.c,v 1.4 2022/04/20 05:37:31 karn Exp $
// Helper functions for iniparser that combine section:key
// April 2022, Phil Karn, KA9Q
#include <iniparser.h>
#include "config.h"

int config_getint(dictionary const *d,char const *section,char const *key,int def){
  if(section == NULL || key == NULL)
    return def;
  char buf[1024];
  snprintf(buf,sizeof(buf),"%s:%s",section,key);
  return iniparser_getint(d,buf,def);
}
float config_getfloat(dictionary const *d,char const *section,char const *key,float def){
  if(section == NULL || key == NULL)
    return def;
  char buf[1024];
  snprintf(buf,sizeof(buf),"%s:%s",section,key);
  return (float)iniparser_getdouble(d,buf,def);
}
double config_getdouble(dictionary const *d,char const *section,char const *key,double def){
  if(section == NULL || key == NULL)
    return def;
  char buf[1024];
  snprintf(buf,sizeof(buf),"%s:%s",section,key);
  return iniparser_getdouble(d,buf,def);
}
int config_getboolean(dictionary const *d,char const *section,char const *key,int def){
  if(section == NULL || key == NULL)
    return def;
  char buf[1024];
  snprintf(buf,sizeof(buf),"%s:%s",section,key);
  return iniparser_getboolean(d,buf,def);
}
char const *config_getstring(dictionary const *d,char const *section,char const *key,char const *def){
  if(section == NULL || key == NULL)
    return def;
  char buf[1024];
  snprintf(buf,sizeof(buf),"%s:%s",section,key);
  return iniparser_getstring(d,buf,def);
}

// Look in dictionary d2 first, fall back to d1 if not found
char const *config2_getstring(dictionary const *d1,dictionary const *d2,char const *sec1,char const *sec2,char const *key,char const *def){
  char const *cp = config_getstring(d2,sec2,key,NULL);
  if(cp)
    return cp;
  cp = config_getstring(d1,sec1,key,NULL);
  if(cp)
    return cp;
  return def;
}
  
int config2_getint(dictionary const *d1,dictionary const *d2,char const *sec1,char const *sec2,char const *key,int def){
  char const *cp = config_getstring(d2,sec2,key,NULL);
  if(cp)
    return strtol(cp,NULL,0);

  cp = config_getstring(d1,sec1,key,NULL);
  if(cp)
    return strtol(cp,NULL,0);    

  return def;
}
float config2_getfloat(dictionary const *d1,dictionary const *d2,char const *sec1,char const *sec2,char const *key,float def){
  char const *cp = config_getstring(d2,sec2,key,NULL);
  if(cp)
    return strtof(cp,0);

  cp = config_getstring(d1,sec1,key,NULL);
  if(cp)
    return strtof(cp,0);    

  return def;
}
double config2_getdouble(dictionary const *d1,dictionary const *d2,char const *sec1,char const *sec2,char const *key,double def){
  char const *cp = config_getstring(d2,sec2,key,NULL);
  if(cp)
    return strtof(cp,0);

  cp = config_getstring(d1,sec1,key,NULL);
  if(cp)
    return strtof(cp,0);    

  return def;
}
int config2_getboolean(dictionary const *d1,dictionary const *d2,char const *sec1,char const *sec2,char const *key,int def){
  int x = config_getboolean(d2,sec2,key,-1);
  if(x != -1)
    return x;

  x = config_getboolean(d1,sec1,key,-1);
  if(x != -1)
    return x;
  return def;
}

  
