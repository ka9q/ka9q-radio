// $Id: config.c,v 1.3 2022/04/05 06:51:18 karn Exp $
// Helper functions for iniparser that combine section:key
// April 2022, Phil Karn, KA9Q
#include <iniparser/iniparser.h>
#include "config.h"

int config_getint(dictionary const *d,char const *section,char const *key,int def){
  char buf[1024];
  snprintf(buf,sizeof(buf),"%s:%s",section,key);
  return iniparser_getint(d,buf,def);

}
float config_getfloat(dictionary const *d,char const *section,char const *key,float def){
  char buf[1024];
  snprintf(buf,sizeof(buf),"%s:%s",section,key);
  return (float)iniparser_getdouble(d,buf,def);

}
double config_getdouble(dictionary const *d,char const *section,char const *key,double def){
  char buf[1024];
  snprintf(buf,sizeof(buf),"%s:%s",section,key);
  return iniparser_getdouble(d,buf,def);

}
int config_getboolean(dictionary const *d,char const *section,char const *key,int def){
  char buf[1024];
  snprintf(buf,sizeof(buf),"%s:%s",section,key);
  return iniparser_getboolean(d,buf,def);

}
char const *config_getstring(dictionary const *d,char const *section,char const *key,char const *def){
  char buf[1024];
  snprintf(buf,sizeof(buf),"%s:%s",section,key);
  return iniparser_getstring(d,buf,def);

}

