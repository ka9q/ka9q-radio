#ifndef _CONFIG_H
#define _CONFIG_H 1

int config_getint(dictionary const *d,char const *section,char const *key,int def);
double config_getdouble(dictionary const *d,char const *section,char const *key,double def);
float config_getfloat(dictionary const *d,char const *section,char const *key,float def);
int config_getboolean(dictionary const *d,char const *section,char const *key,int def);
char const *config_getstring(dictionary const *d,char const *section,char const *key,char const *def);

int config2_getint(dictionary const *d1,dictionary const *d2,char const *sec1,char const *sec2,char const *key,int def);
double config2_getdouble(dictionary const *d1,dictionary const *d2,char const *sec1,char const *sec2,char const *key,double def);
float config2_getfloat(dictionary const *d1,dictionary const *d2,char const *sec1,char const *sec2,char const *key,float def);
int config2_getboolean(dictionary const *d1,dictionary const *d2,char const *sec1,char const *sec2,char const *key,int def);
char const * config2_getstring(dictionary const *d1,dictionary const *d2,char const *sec1,char const *sec2,char const *key,char const *def);

#endif
