#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "avahi.h"


#if 0 // devel testing
int main(){
  char const *service_name = "_ka9q-ctl._udp";

  struct service_tab table[1000];
  int line_count = avahi_browse(table,1000,service_name);
  

  for(int i=0;i<line_count;i++){
    struct service_tab *tp = &table[i];
    printf("line %s interface %s protocol %s name %s type %s domain %s dns %s address %s port %s txt %s\n",
	   tp->line_type,
	   tp->interface,
	   tp->protocol,
	   tp->name,
	   tp->type,
	   tp->domain,
	   tp->dns_name,
	   tp->address,
	   tp->port,
	   tp->txt);
  }
  exit(0);
}
#endif

static int table_compare(void const *a,void const *b){
  struct service_tab const *t1 = a;
  struct service_tab const *t2 = b;
  return strcmp(t1->name,t2->name);
}
// De-escape decimal sequences of the form \032 to (space)
// Do it in place since the string can only get shorter
static int deescape(char *s){
  int len = 0;
  char *rp = s;
  char *wp = s;
  bool add_null = false;

  while(*rp != '\0'){
    if(*rp == '\\'){
      rp++;
      *wp = (*rp++ - '0') * 100;
      *wp += (*rp++ - '0') * 10;
      *wp++ += (*rp++ - '0');
      add_null = true;
    } else {
      *wp++ = *rp++; // Copy original character, which may have shifted
    }
    len++;
  }
  if(add_null)
    *wp = '\0'; // String has shortened, make sure it's null terminated
  return len;
}



static const int linesize = 1024;

// Invoke 'avahi-browse' command, filter and sort output
int avahi_browse(struct service_tab *table,int tabsize,char const *service_name){
  if(service_name == NULL || strlen(service_name) == 0 || table == NULL || tabsize == 0)
    return 0;

  memset(table,0,sizeof(*table) * tabsize); // Clear it out in case we free it later

  FILE *fp;
  {
    char cmd[1024];
    snprintf(cmd,sizeof(cmd),"avahi-browse -ptr %s",service_name);
    fp = popen(cmd,"r");
  }
  if(fp == NULL)
    exit(1);

  int line_count;
  for(line_count = 0; line_count < tabsize;){
    struct service_tab *tp = &table[line_count];
    char *line = malloc(linesize);
    tp->buffer = line; // so it can be freed later
    fgets(line,linesize,fp);
    if(feof(fp))
      break;

    tp->line_type = strsep(&line,";");
    tp->interface = strsep(&line,";");
    tp->protocol =  strsep(&line,";");
    tp->name =  strsep(&line,";");
    tp->type =  strsep(&line,";");
    tp->domain = strsep(&line,";");
    tp->dns_name = strsep(&line,";");
    tp->address = strsep(&line,";");
    tp->port =  strsep(&line,";");
    tp->txt =   strsep(&line,";");

    if(strcmp(tp->line_type,"=") == 0 // Only full lines with resolved addresses
       && strcmp(tp->protocol,"IPv4") == 0 // Only IPv4 for now
       && strcmp(tp->type,service_name) == 0){ // Should always match, but to be sure
      // Keep this line and the pointers into it
      deescape(tp->name);
      tp++;
      line_count++;
    } else {
      free(tp->buffer);
      // tp pointers are now invalid, but they get reset on next iteration or it passes out of scope
    }
  }
  pclose(fp); // What to do with return code?
  printf("%d valid lines read\n",line_count);
  // Sort by instance entity name
  qsort(table,line_count,sizeof(table[0]),table_compare);
  return line_count;
}

// Free and wipe pointers inside service table; caller must free table array itself
void avahi_free_service_table(struct service_tab *table,int tabsize){
  for(int i=0; i < tabsize; i++){
    free(table[i].buffer);
  }
  memset(table,0,sizeof(*table) * tabsize); // Clear it all out
}
