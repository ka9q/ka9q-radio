// Helper functions for iniparser that combine section:key
// April 2022, Phil Karn, KA9Q
// Copyright 2022-2023, Phil Karn, KA9Q

#include <stdio.h>
#include <iniparser/iniparser.h>
#include <stdlib.h>
#include <stdbool.h>
#include "config.h"


// Validate a section of the dictionary against or or two lists
// Return count of bad entries
// If fp != NULL, issue error on specified stream
int config_validate_section(FILE *fp,dictionary const *d,char const *section,char const *list1[],char const *list2[]){
  if(d == NULL || (list1 == NULL && list2 == NULL) || section == NULL)
    return -1;

  int bad = 0;
  int num_keys = iniparser_getsecnkeys(d, section);
  if(num_keys == 0)
    return 0;
  char const *used_keys[num_keys];
  if(iniparser_getseckeys(d, section,used_keys) == NULL){
    if(fp)
      fprintf(fp,"Unknown error reading keys in section [%s]\n",section);
    return -1;
  }
  for (int j = 0; j < num_keys; j++){
    bool matched = false;
    char const *key = used_keys[j];
    char const *cp;
    if((cp = strchr(key,':')) != NULL)
      key = cp+1; // Drop section: prefix on key entry

    if(list1 != NULL){
      for (int k = 0; list1[k] != NULL; k++) {
	if (strcmp(key, list1[k]) == 0){
	  matched = true;
	  break;
	}
      }
    }
    if(!matched && list2 != NULL){
      for (int k = 0; list2[k] != NULL; k++) {
	if (strcmp(key, list2[k]) == 0){
	  matched = true;
	  break;
	}
      }
    }

    if(!matched){
      bad++;
      if(fp != NULL)
	fprintf(fp,"Unknown key \"%s\" in section [%s]\n", key,section);
    }
  }
  return bad;
}
// Validate an entire dictionary against a list
// Return count of bad entries
// If fp != NULL, issue error on specified stream
// This function assumes all keys are valid in all sections
int config_validate(FILE *fp,dictionary const *d,char const *list1[],char const *list2[]){
  if(d == NULL || (list1 == NULL && list2 == NULL))
    return -1;
  int bad = 0;
  int const number_of_sections = iniparser_getnsec(d);  // Number of sections
  for (int i = 0; i < number_of_sections; i++) {
    char const *section = iniparser_getsecname(d, i);
    int c = config_validate_section(fp,d,section,list1,list2);
    if(c < 0)
      return -1;
    bad += c;
  }
  return bad;
}




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
