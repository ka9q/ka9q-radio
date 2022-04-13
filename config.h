#ifndef _CONFIG_H
#define _CONFIG_H 1

int config_getint(dictionary const *d,char const *section,char const *key,int def);
double config_getdouble(dictionary const *d,char const *section,char const *key,double def);
float config_getfloat(dictionary const *d,char const *section,char const *key,float def);
int config_getboolean(dictionary const *d,char const *section,char const *key,int def);
char const *config_getstring(dictionary const *d,char const *section,char const *key,char const *def);
#endif
