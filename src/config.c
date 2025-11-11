// Helper functions for iniparser that combine section:key
// April 2022, Phil Karn, KA9Q
// Copyright 2022-2023, Phil Karn, KA9Q

#include <stdio.h>
#include "compat_iniparser.h"
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
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

static inline int min(int a,int b){
  return a < b ? a : b;
}


// Functions to compute Levenshtein distance - from ChatGPT
#if 1
static int levenshtein_distance(const char *s1, const char *s2) {
  size_t len1 = strlen(s1);
  size_t len2 = strlen(s2);
  int row1[len2 + 1];
  int row2[len2 + 1];

  // Initialize first row
  for (size_t j = 0; j <= len2; j++)
    row1[j] = j;

  for (size_t i = 1; i <= len1; i++) {
    row2[0] = i;
    for (size_t j = 1; j <= len2; j++) {
      int cost = (s1[i - 1] == s2[j - 1]) ? 0 : 1;
      row2[j] = min(row1[j] + 1,        // Deletion
                    min(row2[j - 1] + 1,   // Insertion
                        row1[j - 1] + cost));  // Substitution
    }
    memcpy(row1, row2, (len2 + 1) * sizeof(int));  // Copy new row
  }
  return row1[len2];
}
#else
static int levenshtein_distance(const char *s1, const char *s2) {
  size_t len1 = strlen(s1);
  size_t len2 = strlen(s2);
  int matrix[len1 + 1][len2 + 1];

  // Initialize the matrix
  for (size_t i = 0; i <= len1; i++)
    matrix[i][0] = i;
  for (size_t j = 0; j <= len2; j++)
    matrix[0][j] = j;

  // Fill the matrix
  for (size_t i = 1; i <= len1; i++) {
    for (size_t j = 1; j <= len2; j++) {
      int cost = (s1[i - 1] == s2[j - 1]) ? 0 : 1;
      matrix[i][j] = min(matrix[i - 1][j] + 1,        // Deletion
			  min(matrix[i][j - 1] + 1,   // Insertion
			       matrix[i - 1][j - 1] + cost));  // Substitution
    }
  }
  return matrix[len1][len2];
}
#endif

// Function to validate key and suggest corrections for likely misspellings
static const char *suggest_correction(const char *input_key, const char *valid_keys[]) {
  if(input_key == NULL || valid_keys == NULL || strlen(input_key) == 0)
    return NULL;
  int best_distance = 3;  // Set a threshold distance for possible typos
  const char *best_match = NULL;

  for (size_t i = 0; valid_keys[i] != NULL; i++) {
    if(strlen(valid_keys[i]) == 0)
      continue; // Don't match any empty entries (which shouldn't exist)
    if(strcmp(input_key,valid_keys[i]) == 0)
      return input_key; // Found as is
    int distance = levenshtein_distance(input_key, valid_keys[i]);
    if (distance < best_distance) {
      best_distance = distance;
      best_match = valid_keys[i];
    }
  }
  return best_match ? best_match : NULL;
}

// Validate a section of the dictionary against or or two lists
// Return count of bad entries
// If fp != NULL, issue error on specified stream
int config_validate_section(FILE *fp,dictionary const *d,char const *section,char const *list1[],char const *list2[]){
  if(fp == NULL || d == NULL || (list1 == NULL && list2 == NULL) || section == NULL)
    return -1;

  int num_keys = iniparser_getsecnkeys(d, section);
  if(num_keys == 0)
    return 0;
  char const *used_keys[num_keys];
  if(iniparser_getseckeys(d, section,used_keys) == NULL){
    fprintf(fp,"Unknown error reading keys in section [%s]\n",section);
    return -1;
  }
  char const **list = NULL;
  if(list1 != NULL && list2 != NULL){
    // Concatenate lists (there should be a cleaner way to do all this)
    // Count entries on both lists
    int count = 0;
    for(int i=0; list1[i] != NULL; i++)
      count++;
    for(int i=0; list2[i] != NULL; i++)
      count++;
    count++; // for ending null
    list = alloca(count * sizeof(char *));

    count = 0;
    for(int i = 0; list1[i] != NULL; i++)
      list[count++] = list1[i];
    for(int i = 0; list2[i] != NULL; i++)
      list[count++] = list2[i];

    list[count++] = NULL;
  } else
    list = (list1 != NULL) ? list1 : list2;

  assert(list != NULL);

  int bad = 0;
  for (int j = 0; j < num_keys; j++){
    char const *key = used_keys[j];
    char const *cp;
    if((cp = strchr(key,':')) != NULL)
      key = cp+1; // Drop section: prefix on key entry

    char const *best_match = suggest_correction(key, list);
    if(best_match == NULL){
      fprintf(fp,"[%s] key \"%s\" not found\n",section,key);
      bad++;
    } else if(strcmp(best_match,key) != 0){
      bad++;
      fprintf(fp,"[%s] key \"%s\": did you mean \"%s\"?\n",section,key,best_match);
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
