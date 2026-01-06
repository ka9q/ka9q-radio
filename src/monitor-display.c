// Control plane section of the multicast monitor program
// Moved out of monitor.c when it was getting way too big
// Copyright Aug 2024 Phil Karn, KA9Q

#define _GNU_SOURCE 1
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include <opus/opus.h>
#include <portaudio.h>
#include <ncurses.h>
#include <locale.h>
#include <signal.h>
#include <getopt.h>
#include <iniparser/iniparser.h>
#if __linux__
#include <bsd/string.h>
#include <alsa/asoundlib.h>
#else
#include <string.h>
#endif
#include <sysexits.h>
#include <poll.h>
#include <regex.h>

#include "conf.h"
#include "config.h"
#include "misc.h"
#include "multicast.h"
#include "radio.h"
#include "iir.h"
#include "morse.h"
#include "status.h"
#include "monitor.h"

bool Auto_sort = false;
int Update_interval = 100;  // Default time in ms between display updates

// Simple hack database of identifiers for specific frequencies
// Return an ascii string identifier indexed by ssrc
// Database in /usr/share/ka9q-radio/id.txt
// Really should be rewritten with something much better
struct idtable {
  double freq; // rounded to 1 Hz
  double tone;
  char id[128];
};
#define IDSIZE 1024
static int Nid;
static struct idtable Idtable[IDSIZE];
static struct stat Last_stat;

static void update_monitor_display(void);
static void process_keyboard(void);
static int render(int const row, int const col, char const scratch[LINES][COLS],int const nrows,int const ncols);
static int render_left(int const row, int const col, char const scratch[LINES][COLS],int const nrows,int const ncols);

static int First_session = 0;
static int Sessions_per_screen = 0;
static int Current = 0;
static bool help = false;

struct session *Sess_ptr[NSESSIONS];

// Versions of ncurses routines that truncate at EOL
// Remaining problem: if I use the last column, the position will still
// wrap to the first column of the next row, and I can't stop that except by not using the last column.
// Return number of chars written

int mvprintwt(int row,int col,char const *fmt,...){
  if(col < 0)
    return ERR;

  int space = COLS - col - 1; // leave last column open
  if(space <= 0)
    return ERR;

  va_list ap;
  va_start(ap,fmt);
  char string[COLS+1]; // Maximum line length plus null
  int r = vsnprintf(string,space,fmt,ap); // write only 'space' characters, space <= COLS
  va_end(ap);
  if(r < 0)
    return ERR;
  mvaddstr(row,col,string);
  return r;
}

// Same for printw() - truncate at EOL
// returns number of chars written
int printwt(char const *fmt,...){
  int row,col;
  getyx(stdscr,row,col);
  if(col < 0)
    return ERR;

  int space = COLS - col - 1; // leave last column open
  if(space <= 0)
    return ERR;

  va_list ap;
  va_start(ap,fmt);
  char string[COLS+1]; // Maximum line length plus null
  int r = vsnprintf(string,space,fmt,ap); // write only 'space' characters, space <= COLS
  va_end(ap);
  if(r < 0)
    return ERR;
  mvaddstr(row,col,string);
  return r;
}

// Same for mvaddstr() and addstr()
int mvaddstrt(int row,int col,char const *str){
  size_t space = COLS - col - 1; // Leave last column open
  if(strlen(str) <= space)
    return mvaddstr(row,col,str); // fits
  char temp[space+1];
  strlcpy(temp,str,sizeof temp); // truncate
  return mvaddstr(row,col,temp);
}
int addstrt(char const *str){
  int row,col;
  getyx(stdscr,row,col);
  size_t space = COLS - col - 1; // Leave last column open
  if(strlen(str) <= space)
    return mvaddstr(row,col,str); // fits
  char temp[space+1];
  strlcpy(temp,str,sizeof(temp)); // truncate
  return mvaddstr(row,col,temp);
}

// Use ncurses to display streams
void *display(void *arg){
  pthread_setname("display");
  (void)arg; // unused

  if(initscr() == NULL){
    fprintf(stderr,"initscr() failed, disabling control/display thread\n");
    pthread_exit(NULL);
  }
  keypad(stdscr,TRUE);
  timeout(Update_interval);
  cbreak();
  noecho();

  // Build list of pointers into Sessions array
  for(int i=0; i < NSESSIONS; i++)
    Sess_ptr[i] = Sessions + i;

  while(!atomic_load_explicit(&Terminate,memory_order_acquire)){
    // Start screen update
    move(0,0);
    clrtobot();
    if(Source == NULL)
      addstrt("KA9Q Multicast Audio Monitor:");
    else
      printwt("KA9Q Multicast Audio Monitor, only from %s:",Source);

    for(int i=0;i<Nfds;i++)
      printwt(" %s",Mcast_address_text[i]);
    int row,col;
    getyx(stdscr,row,col);
    if(col != 0){
      col = 0;
      move(++row,col);
    }

    if(help){
      char path [PATH_MAX];
      dist_path(path,sizeof(path),"monitor-help.txt");
      FILE *fp = fopen(path,"r");
      if(fp != NULL){
	size_t size = 0;
	char *line = NULL;
	while(getline(&line,&size,fp) > 0)
	  addstrt(line);

	FREE(line);
	fclose(fp);
	fp = NULL;
      }
    }
    if(Quiet_mode){
      addstrt("Hit 'q' to resume screen updates\n");
    } else
      update_monitor_display();

    process_keyboard();
  }
  return NULL;
}
// sort callback for sort_session_active() for comparing sessions by most recently active (or currently longest active)
static int scompare(void const *a, void const *b){
  struct session const * const s1 = *(struct session **)a;
  struct session const * const s2 = *(struct session **)b;

  if(s1 == s2) // same session compares equal (shouldn't happen)
    return 0;
  if(s1 == NULL || !inuse(s1)){ // first is empty or idle, put it at end
    if(s2 == NULL || !inuse(s1))
      return 0;
    else
      return +1;
  } else if(s2 == NULL || !inuse(s2)){
    return -1; // s1 is not empty
  }
  // Both in use. Fuzz needed because active sessions are updated when packets arrive
  if(s1->active != 0 && s2->active != 0){
    if(fabs(s1->active - s2->active) < 1.0) {
      return s1->ssrc > s2->ssrc ? +1 : -1; // resolve ties by ssrc to stop rapid flipping
    } else if(s1->active > s2->active){
      return -1; // s1 Longer active, move to top
    } else {
      return +1; // s2 longer active, move it to the top
    }
  } else {
    if(s2->active != 0)
      return +1;   // s1 inctive
    else if(s1->active != 0)
      return -1; // s2 inactive
    else     // Both inactive, sort by last active times
      return s1->last_active > s2->last_active ? -1 : +1;    // last_active is in nanoseconds so chances of equality are nil
  }
}

// sort callback for sort_session() for comparing sessions by total time
static int tcompare(void const *a, void const *b){
  struct session const * const s1 = *(struct session **)a;
  struct session const * const s2 = *(struct session **)b;

  if(s1 == s2) // same session compares equal (shouldn't happen)
    return 0;
  if(s1 == NULL || !inuse(s1)) // first is empty or idle, put it at end
    return +1;
  if(s2 == NULL || !inuse(s2)) // second is empty or idle, put it at end
    return -1;

  if(fabs(s1->tot_active - s2->tot_active) < 1.0) // equal within margin
    return s1->ssrc > s2->ssrc ? +1 : -1; // resolve ties by ssrc to stop rapid flipping
  if(s1->tot_active > s2->tot_active)
    return -1;
  return +1;
}

// Defragment session list
static int defragment_session(void){
  pthread_mutex_lock(&Sess_mutex);
  for(int i=0; i < NSESSIONS-1; i++){
    // Find next unused slot
    if(inuse(Sess_ptr[i]))
      continue;

    // Find following used slot
    int j = i+1;
    while(j < NSESSIONS && !inuse(Sess_ptr[j]))
      j++;

    if(j == NSESSIONS)
      break;
    struct session *save = Sess_ptr[i];
    Sess_ptr[i] = Sess_ptr[j];
    Sess_ptr[j] = save;
  }
  pthread_mutex_unlock(&Sess_mutex);
  return 0;
}


// Sort session list in increasing order of age
static int sort_session_active(void){
  pthread_mutex_lock(&Sess_mutex);
  qsort(Sess_ptr,NSESSIONS,sizeof(Sess_ptr[0]),scompare);
  pthread_mutex_unlock(&Sess_mutex);
  return 0;
}
static int sort_session_total(void){
  pthread_mutex_lock(&Sess_mutex);
  qsort(Sess_ptr,NSESSIONS,sizeof(Sess_ptr[0]),tcompare);
  pthread_mutex_unlock(&Sess_mutex);
  return 0;
}

static int id_compare(const void *a,const void *b){
  struct idtable const *p1 = (struct idtable *)a;
  struct idtable const *p2 = (struct idtable *)b;
  if(p1 == NULL || p2 == NULL)
    return 0; // Shouldn't happen
  // Compare frequencies first, then tones
  if(p1->freq < p2->freq)
    return -1;
  if(p1->freq > p2->freq)
    return +1;
  if(p1->tone < p2->tone)
    return -1;
  if(p1->tone > p2->tone)
    return +1;
  return 0;
}


void load_id(void){
  char filename[PATH_MAX];
  dist_path(filename,sizeof(filename),ID);
  struct stat statbuf;
  stat(filename,&statbuf);
  if(statbuf.st_mtime != Last_stat.st_mtime)
    Nid = 0; // Force reload

  if(Nid != 0)
    return;

  // Lines consist of frequency, PL tone, id. The PL tone is optional, but must be of form 88.3 or 114.8
  // double escape special chars to get single backslashes through the C compiler
  char const *pattern1 = "([0-9\\.]+)[[:space:]]+([0-9]{2,3}\\.[0-9])[[:space:]]+(.*)";
  char const *pattern2 = "([0-9\\.]+)[[:space:]]+(.*)";
  regex_t preg1;
  int r = regcomp(&preg1,pattern1,REG_EXTENDED);
  if(r != 0){
    char errbuf[256];
    regerror(r,&preg1,errbuf,sizeof(errbuf));
    fprintf(stderr,"regex compile(%s) failed: %s\n",pattern1,errbuf);
    return;
  }
  regex_t preg2;
  r = regcomp(&preg2,pattern2,REG_EXTENDED);
  if(r != 0){
    char errbuf[256];
    regerror(r,&preg2,errbuf,sizeof(errbuf));
    fprintf(stderr,"regex compile(%s) failed: %s\n",pattern2,errbuf);
    return;
  }
  // Load table
  FILE * const fp = fopen(filename,"r");
  if(fp == NULL)
    return;

  char *line = NULL;
  size_t linesize = 0;
  while(getline(&line,&linesize,fp) > 0){
    chomp(line);

    if(line[0] == '#' || strlen(line) == 0)
      continue; // Comment
    assert(Nid < IDSIZE);
#define NMATCH (4)
    regmatch_t pmatch[NMATCH];
    // Look for line with PL tone
    r = regexec(&preg1,line,NMATCH,pmatch,0);
#if 0
    if(r != 0){
      char errbuf[256];
      regerror(r,&preg1,errbuf,sizeof(errbuf));
      fprintf(stderr,"regex1 on %s failed: %s\n",line,errbuf);
    }
#endif
    if(r == 0){
      // field 1: frequency
      char freq[128];
      memset(freq,0,sizeof(freq));
      memcpy(freq,&line[pmatch[1].rm_so],pmatch[1].rm_eo - pmatch[1].rm_so);
      char *ptr = NULL;
      Idtable[Nid].freq = (long)round(strtod(freq,&ptr));
      if(ptr == freq)
	continue; // no parseable number

      // field 2: PL tone
      // Because it's optional, it must be of the form nn.n or nnn.n
      if(pmatch[2].rm_so != -1){
	memset(freq,0,sizeof(freq));
	memcpy(freq,&line[pmatch[2].rm_so],pmatch[2].rm_eo - pmatch[2].rm_so);
	Idtable[Nid].tone = strtod(freq,&ptr);
	if(ptr == freq)
	  continue; // no parseable number
      }
      if(pmatch[3].rm_so != -1){
	// Free-form ID field
	strlcpy(Idtable[Nid].id,&line[pmatch[3].rm_so],sizeof(Idtable[Nid].id));
      }
    } else if((r = regexec(&preg2,line,NMATCH,pmatch,0)) == 0){
      // Line without tone entry
      char freq[128];
      memset(freq,0,sizeof(freq));
      memcpy(freq,&line[pmatch[1].rm_so],pmatch[1].rm_eo - pmatch[1].rm_so);
      char *ptr = NULL;
      Idtable[Nid].freq = round(strtod(freq,&ptr));
      if(ptr == freq)
	continue; // no parseable number

      if(pmatch[2].rm_so != -1){
	// Free-form ID field
	strlcpy(Idtable[Nid].id,&line[pmatch[2].rm_so],sizeof(Idtable[Nid].id));
	// Make sure it's null terminated
	Idtable[Nid].id[sizeof(Idtable[Nid].id)-1] = '\0';
      }
    }
# if 0
 else {
      char errbuf[256];
      regerror(r,&preg2,errbuf,sizeof(errbuf));
      fprintf(stderr,"regex2 on %s failed: %s\n",line,errbuf);
      continue;
    }
#endif
    Nid++;
    if(Nid == IDSIZE){
      fprintf(stderr,"ID table overlow, size %d\n",Nid);
      break;
    }
  }
  fclose(fp);
  if(Nid > 0)
    qsort(Idtable,Nid,sizeof(Idtable[0]),id_compare);

  regfree(&preg1);
  regfree(&preg2);
  FREE(line);
}

// Use binary search to speed things up since we do this more often
char const *lookupid(double freq,double tone){
  struct idtable key;
  key.freq = round(freq);
  key.tone = tone;

  struct idtable *entry = (struct idtable *)bsearch(&key,Idtable,Nid,sizeof(key),id_compare);
  if(entry == NULL)
    return NULL;
  else
    return entry->id;
}
static void update_monitor_display(void){
  int row,col;
  getyx(stdscr,row,col);

  // First header line
  if(Repeater_tail != 0){
    if(Last_id_time != 0)
      printwt("Last ID: %.1lf sec",((double)(gps_time_ns() - Last_id_time) * 1e-9));
    if(PTT_state)
      addstrt(" PTT On");
    else if(Last_xmit_time != 0)
      printwt(" PTT Off; Last xmit: %.1lf sec",(double)(gps_time_ns() - Last_xmit_time) * 1e-9);
    col = 0; // next line
    move(++row,col);
  }

  if(Constant_delay)
    printwt("Constant delay ");

  if(Start_muted)
    printwt("**Starting new sessions muted** ");

  if(Voting)
    printwt("SNR Voting enabled");

  getyx(stdscr,row,col);
  if(col != 0){
    col = 0;
    move(++row,col);
  }
  int64_t rptr = atomic_load_explicit(&Output_time,memory_order_relaxed);
  if(Verbose){
    // Measure skew between sampling clock and UNIX real time (hopefully NTP synched)
    double const pa_seconds = Pa_GetStreamTime(Pa_Stream) - Start_pa_time;

    uint64_t audio_frames = atomic_load_explicit(&Audio_frames,memory_order_relaxed);
    double const rate = audio_frames / pa_seconds;

    printwt("%s playout %.0lf ms latency %5.1lf ms D/A %'.1lf Hz",
	    opus_get_version_string(),1000*Playout,1000*Portaudio_delay,rate);
    printwt(" (%+4.0lf ppm)",1e6 * (rate / DAC_samprate - 1));
    // Time since last packet drop on any channel
    printwt(" EFS %'.1lf",(1e-9*(gps_time_ns() - Last_error_time)));
    //    int64_t total = atomic_load(&Output_total);
    //    int64_t calls = atomic_load(&Callbacks);
    int quant = atomic_load_explicit(&Callback_quantum,memory_order_relaxed);
    double level = atomic_load_explicit(&Output_level,memory_order_relaxed);
    level = power2dB(level);
    printwt(" Clock %.1lfs %.1lf dBFS CB N %u",(double)rptr/DAC_samprate,level,quant);
    extern int Session_creates;
    printwt(" sessions %d",Session_creates);
  }
  getyx(stdscr,row,col);
  if(col != 0){
    col = 0;
    move(++row,col);
  }
  Sessions_per_screen = LINES - getcury(stdscr) - 1;

  defragment_session();
  if(Auto_sort)
    sort_session_total();

  if(First_session >= NSESSIONS)
    First_session = NSESSIONS-1;
  while(First_session > 0 && !inuse(Sess_ptr[First_session]))
    First_session--;

  // Show channel statuses
  int header_line = row;
  int col_save = col;
  int first_line = header_line+1; // after header line

  // Bound current pointer to active list area
  while(Current > 0 && !inuse(Sess_ptr[Current]))
    Current--; // Current session is no valid, back up

  if(Current <= First_session)
    Current = First_session;
  else if(Current > LINES - first_line)
    Current = LINES - first_line;

  // dB column
  int width = 4;
  mvprintwt(row++,col,"%*s",width,"dB");
  for(int session = First_session; session < NSESSIONS &&  row < LINES; session++,row++){
    struct session const *sp = Sess_ptr[session];
    if(!inuse(sp)) break;
    mvprintwt(row,col,"%+*.0lf",width,muted(sp) ? -INFINITY : voltage2dB(sp->gain));
  }
  col += width;
  row = header_line;
  if(Auto_position){
    if(col >= COLS)
      goto done;

    // Pan column
    width = 4;
    mvprintwt(row++,col," pan");
    for(int session = First_session; session < NSESSIONS && row < LINES; session++,row++){
      struct session const *sp = Sess_ptr[session];
      if(!inuse(sp)) break;
      mvprintwt(row,col,"%*d",width,(int)round(100*sp->pan));
    }
    col += width;
    row = header_line;
  }
  // SSRC
  if(col >= COLS)
    goto done;

  {  // ssrc
    char scratch [LINES][COLS];
    memset(scratch, 0 , sizeof scratch);
    int session = First_session;
    int width = 20;
    int rows = 0;
    snprintf(scratch[rows++],COLS,"%*s",width,"ssrc");
    for(; rows < LINES && session < NSESSIONS; rows++,session++){
      struct session const *sp = Sess_ptr[session];
      if(!inuse(sp))
	break;

      snprintf(scratch[rows],COLS,"%*u",width,sp->ssrc);
    }
    col++;
    width = render(header_line,col,scratch,rows,width);
    col += width;
  }
  if(col >= COLS)
    goto done;

  if(Notch){
    width = 7;
    mvprintwt(row++,col,"%*s",width,"tone");
    for(int session = First_session; session < NSESSIONS && row < LINES; session++,row++){
      struct session const *sp = Sess_ptr[session];
      if(!inuse(sp))
	break;
      if(sp->notch_enable && sp->notch_tone != 0)
	mvprintwt(row,col,"%*.1f%c",width-1,sp->notch_tone,sp->current_tone == sp->notch_tone ? '*' : ' ');

    }
    col += width;
    row = header_line;
  }
  if(col >= COLS)
    goto done;

  {  // freq
    char scratch [LINES][COLS];
    memset(scratch,0,sizeof scratch);
    int session = First_session;
    int width = 30;
    int rows = 0;

    snprintf(scratch[rows++],COLS,"%*s",width,"freq");
    for(;rows < LINES && session < NSESSIONS; rows++,session++){
      struct session const *sp = Sess_ptr[session];
      if(!inuse(sp)) break;

      snprintf(scratch[rows],COLS,"%'*.0lf",width,sp->chan.tune.freq);
    }
    col++;
    width = render(header_line,col,scratch,rows,width);
    col += width;
  }
  if(col >= COLS)
    goto done;

  {  // mode
    char scratch [LINES][COLS];
    memset(scratch,0,sizeof scratch);
    int session = First_session;
    int width = COLS;
    int rows = 0;

    snprintf(scratch[rows++],COLS,"%s","mode");
    for(;rows < LINES && session < NSESSIONS; rows++,session++){
      struct session const *sp = Sess_ptr[session];
      if(!inuse(sp)) break;

      snprintf(scratch[rows],COLS,"%s",sp->chan.preset);
    }
    col++;
    width = render_left(header_line,col,scratch,rows,width);
    col += width;
  }
  if(col >= COLS)
    goto done;

  width = 6;
  mvprintwt(row++,col,"%*s",width-1,"s/n");
  for(int session = First_session; session < NSESSIONS && row < LINES; session++,row++){
    struct session const *sp = Sess_ptr[session];
    if(inuse(sp) && isfinite(sp->snr))
      mvprintwt(row,col,"%*.1f%c",width-1,sp->snr,(Voting && sp == Best_session) ? '*' : ' ');
  }
  col += width;
  row = header_line;

  col++; // ID is left justified, add a leading space
  if(col >= COLS)
    goto done;

  // Dynamic column width
  {  // id
    char scratch [LINES][COLS];
    memset(scratch,0,sizeof scratch);
    int session = First_session;
    int width = COLS;
    int rows = 0;
    bool enable = false;

    snprintf(scratch[rows++],COLS,"%s","id");
    for(;rows < LINES && session < NSESSIONS; rows++,session++){
      struct session const *sp = Sess_ptr[session];
      if(!inuse(sp)) break;

      if(strlen(sp->id) > 0){
	snprintf(scratch[rows],COLS,"%s",sp->id);
	enable = true;
      }
    }
    if(enable){
      col++;
      width = render_left(header_line,col,scratch,rows,width);
      col += width;
    }
  }
  if(col >= COLS)
    goto done;

  {  // total active time
    char scratch [LINES][COLS];
    memset(scratch, 0 , sizeof scratch);
    int session = First_session;
    int width = 20;
    int rows = 0;
    snprintf(scratch[rows++],COLS,"%*s",width,"Tot");
    for(; rows < LINES && session < NSESSIONS; rows++,session++){
      struct session const *sp = Sess_ptr[session];
      if(!inuse(sp))
	break;

      char total_buf[100] = {0};
      ftime(total_buf,sizeof(total_buf),(int64_t)round(sp->tot_active));
      snprintf(scratch[rows],COLS,"%*s",width,total_buf);
    }
    col++;
    width = render(header_line,col,scratch,rows,width);
    col += width;
  }
  if(col >= COLS)
    goto done;

  {
    char scratch [LINES][COLS];
    memset(scratch, 0 , sizeof scratch);
    int session = First_session;
    int width = 20;
    int rows = 0;
    snprintf(scratch[rows++],COLS,"%*s",width,"Cur");
    for(; rows < LINES && session < NSESSIONS; rows++,session++){
      struct session const *sp = Sess_ptr[session];
      if(!inuse(sp))
	break;

      double t = sp->active != 0 ? sp->active : (double)(gps_time_ns() - sp->last_active) * 1e-9;
      char buf[100];
      snprintf(scratch[rows],COLS,"%*s",width,ftime(buf,sizeof buf,t));
    }
    col++;
    width = render(header_line,col,scratch,rows,width);
    col += width;
  }

  if(col >= COLS)
    goto done;
  width = 6;
  mvprintwt(row++,col,"%*s",width,"level");
  for(int session = First_session; session < NSESSIONS && row < LINES; session++,row++){
    struct session const *sp = Sess_ptr[session];
    if(!inuse(sp))
      continue;

    double dB = power2dB(sp->level);
    if(dB >= -99)
      mvprintwt(row,col,"%*.1lf",width,dB);   // Time idle since last transmission

  }
  col += width;
  row = header_line;
  if(col >= COLS)
    goto done;

  width = 8;
  mvprintwt(row++,col,"%*s",width,"Q");
  for(int session = First_session; session < NSESSIONS && row < LINES; session++,row++){
    struct session const *sp = Sess_ptr[session];
    if(!inuse(sp) || qlen(sp) <= 0)
      continue;

    mvprintwt(row,col,"%*d",width,(int)round(1000. * qlen(sp)/DAC_samprate));   // Time idle since last transmission
  }
  col += width;
  row = header_line;

  // Opus/pcm
  col++; // Left justified, add a space
  if(col >= COLS)
    goto done;

  width = 0;
  for(int session = First_session; session < NSESSIONS; session++){
    struct session const *sp = Sess_ptr[session];
    if(!inuse(sp)) break;
    int i = strlen(encoding_string(sp->pt_table[sp->type].encoding));
    if(i > width)
      width = i;
  }

  mvprintwt(row++,col,"%s","type");
  for(int session = First_session; session < NSESSIONS && row < LINES; session++,row++){
    struct session const *sp = Sess_ptr[session];
    if(!inuse(sp)) break;
    mvprintwt(row,col,"%s",encoding_string(sp->pt_table[sp->type].encoding));
  }
  col += width;
  row = header_line;

  // frame size, ms
  if(col >= COLS)
    goto done;
  width = 3;
  mvprintwt(row++,col,"%*s",width,"ms");
  for(int session = First_session; session < NSESSIONS && row < LINES; session++,row++){
    struct session const *sp = Sess_ptr[session];
    if(!inuse(sp)) break;
    if(sp->samprate != 0)
      mvprintwt(row,col,"%*d",width,(1000 * sp->last_framesize/sp->samprate)); // frame size, ms

  }
  col += width;
  row = header_line;

  // channels
  if(col >= COLS)
    goto done;
  width = 2;
  mvprintwt(row++,col,"%*s",width,"c");
  for(int session = First_session; session < NSESSIONS && row < LINES; session++,row++){
    struct session const *sp = Sess_ptr[session];
    if(!inuse(sp))
      break;
    enum encoding encoding = sp->pt_table[sp->type].encoding;
    if(encoding == OPUS || encoding == OPUS_VOIP)
      mvprintwt(row,col,"%*d",width,sp->opus_channels); // actual number in incoming stream
    else
      mvprintwt(row,col,"%*d",width,sp->channels);

  }
  col += width;
  row = header_line;

  // BW
  if(col >= COLS)
    goto done;
  width = 3;
  mvprintwt(row++,col,"%*s",width,"bw");
  for(int session = First_session; session < NSESSIONS && row < LINES; session++,row++){
    struct session const *sp = Sess_ptr[session];
    if(!inuse(sp)) break;
    mvprintwt(row,col,"%*d",width,sp->bandwidth/1000); // convert to kHz
  }
  col += width;
  row = header_line;

  // RTP payload type
  if(col >= COLS)
    goto done;
  width = 4;
  mvprintwt(row++,col,"%*s",width,"pt");
  for(int session = First_session; session < NSESSIONS && row < LINES; session++,row++){
    struct session const *sp = Sess_ptr[session];
    if(!inuse(sp)) break;
    mvprintwt(row,col,"%*d",width,sp->type);
  }
  col += width;
  row = header_line;

  // Data rate, kb/s
  if(col >= COLS)
    goto done;
  width = 6;
  mvprintwt(row++,col,"%*s",width,"rate");
  for(int session = First_session; session < NSESSIONS && row < LINES; session++,row++){
    struct session const *sp = Sess_ptr[session];
    if(!inuse(sp)) break;
    mvprintwt(row,col,"%*.*f", width, sp->datarate < 1e6 ? 1 : 0, .001 * sp->datarate); // decimal only if < 1000
  }
  col += width;
  row = header_line;
  if(col >= COLS)
    goto done;

#if 0
  {  // Processing delay, assuming synchronized system clocks
    char scratch [LINES][COLS];
    memset(scratch, 0 , sizeof scratch);
    int session = First_session;
    int width = 20;
    int rows = 0;
    snprintf(scratch[rows++],COLS,"%*s",width,"Delay");
    for(; rows < LINES && session < NSESSIONS; rows++,session++){
      struct session const *sp = Sess_ptr[session];
      if(!inuse(sp))
	break;

      if(sp->chan.output.rtp.timestamp == 0)
	continue;
      // sp->frontend.timestamp (GPS time at front end) and sp->chan.output.rtp.timestamp (next RTP timestamp to be sent) are updated periodically by status packets
      // sp->last_timestamp contains most recent RTP packet processed
      // This needs further thought and cleanup
      if(gps_time_ns() >  sp->last_active + BILLION/2)
	continue;

      double delay = (double)(int32_t)(sp->chan.output.rtp.timestamp - sp->last_timestamp) / sp->samprate;
      delay += 1.0e-9 * (gps_time_ns() - sp->chan.clocktime);
      snprintf(scratch[rows],COLS,"%'*.3lf", width,delay);
    }
    // Suppress entirely unless there's at least one entry
    col++;
    width = render(header_line,col,scratch,rows,width);
    col += width;
  }
  if(col >= COLS)
    goto done;
#endif

#if 0
  {
    // T0: DAC clock time (ms) at which the RTP timestamp was 0
    // At 48 khz, the 32-bit RTP timestamp wraps in 89478.485 sec or 1.0356306 days
    // should be invariant +/- 1 frame (20 ms) for a given stream unless monitor or radiod is restarted
    // Checks for failures to update timestamp at sender, etc
    char scratch [LINES][COLS];
    memset(scratch, 0 , sizeof scratch);
    int session = First_session;
    int width = 20;
    int rows = 0;
    snprintf(scratch[rows++],COLS,"%*s",width,"T0");
    for(; rows < LINES && session < NSESSIONS; rows++,session++){
      struct session const *sp = Sess_ptr[session];
      if(!inuse(sp))
	break;

      if(sp->samprate == 0)
	continue;

      int64_t wptr = atomic_load_explicit(&sp->wptr,memory_order_relaxed);
      int64_t t0 = 1000*wptr/DAC_samprate - 1000*(int64_t)sp->next_timestamp / sp->samprate;
      snprintf(scratch[rows],COLS,"%'*lld", width, t0); // ms
    }
    col++;
    width = render(header_line,col,scratch,rows,width);
    col += width;
  }

  if(col >= COLS)
    goto done;
#endif
  {
    char scratch [LINES][COLS];
    memset(scratch, 0 , sizeof scratch);
    int session = First_session;
    int width = 20;
    int rows = 0;

    snprintf(scratch[rows++],COLS,"%*s",width,"pkt");
    for(;rows < LINES && session < NSESSIONS; rows++,session++){
      struct session const *sp = Sess_ptr[session];
      if(!inuse(sp)) break;
      snprintf(scratch[rows],COLS,"%*llu",width,(unsigned long long)sp->packets);
    }
    col++;
    width = render(header_line,col,scratch,rows,width);
    col += width;
  }
  if(col >= COLS)
    goto done;

  {
    char scratch [LINES][COLS];
    memset(scratch, 0 , sizeof scratch);
    int session = First_session;
    int width = 10;
    int rows = 0;
    bool enable = false;

    snprintf(scratch[rows++],COLS,"%*s",width,"rst");
    for(;rows < LINES && session < NSESSIONS; rows++,session++){
      struct session const *sp = Sess_ptr[session];
      if(!inuse(sp)) break;
      if(sp->resets > 0)
	enable = true;

      snprintf(scratch[rows],COLS,"%*llu",width,(unsigned long long)sp->resets);
    }
    if(enable){
      col++;
      width = render(header_line,col,scratch,rows,width);
      col += width;
    }
  }
  if(col >= COLS)
    goto done;
  {  // drops
    char scratch [LINES][COLS];
    memset(scratch, 0 , sizeof scratch);
    int session = First_session;
    int width = 10;
    int rows = 0;
    bool enable = false;

    snprintf(scratch[rows++],COLS,"%*s",width,"drp");
    for(;rows < LINES && session < NSESSIONS; rows++,session++){
      struct session const *sp = Sess_ptr[session];
      if(!inuse(sp)) break;
      if(sp->drops > 0)
	enable = true;
      snprintf(scratch[rows],COLS,"%*llu",width,(unsigned long long)sp->drops);
    }
    if(enable){
      col++;
      width = render(header_line,col,scratch,rows,width);
      col += width;
    }
  }
  if(col >= COLS)
    goto done;

  {  // lates
    char scratch [LINES][COLS];
    memset(scratch, 0 , sizeof scratch);
    int session = First_session;
    int width = 10;
    int rows = 0;
    bool enable = false;

    snprintf(scratch[rows++],COLS,"%*s",width,"late");
    for(;rows < LINES && session < NSESSIONS; rows++,session++){
      struct session const *sp = Sess_ptr[session];
      if(!inuse(sp)) break;
      if(sp->lates > 0)
	enable = true;
      snprintf(scratch[rows],COLS,"%*llu",width,(unsigned long long)sp->lates);
    }
    if(enable){
      col++;
      width = render(header_line,col,scratch,rows,width);
      col += width;
    }
  }
  if(col >= COLS)
    goto done;
  {  // earlies
    char scratch [LINES][COLS];
    memset(scratch, 0 , sizeof scratch);
    int session = First_session;
    int width = 10;
    int rows = 0;
    bool enable = false;

    snprintf(scratch[rows++],COLS,"%*s",width,"early");
    for(;rows < LINES && session < NSESSIONS; rows++,session++){
      struct session const *sp = Sess_ptr[session];
      if(!inuse(sp)) break;
      if(sp->lates > 0)
	enable = true;
      snprintf(scratch[rows],COLS,"%*llu",width,(unsigned long long)sp->earlies);
    }
    if(enable){
      col++;
      width = render(header_line,col,scratch,rows,width);
      col += width;
    }
  }

  if(col >= COLS)
    goto done;

  {  // reseq
    char scratch [LINES][COLS];
    memset(scratch, 0 , sizeof scratch);
    int session = First_session;
    int width = 10;
    int rows = 0;
    bool enable = false;

    snprintf(scratch[rows++],COLS,"%*s",width,"resq");
    for(;rows < LINES && session < NSESSIONS; rows++,session++){
      struct session const *sp = Sess_ptr[session];
      if(!inuse(sp)) break;
      snprintf(scratch[rows],COLS,"%*llu",width,(unsigned long long)sp->reseqs);
      if(sp->reseqs > 0)
	enable = true;
    }
    if(enable){
      col++;
      width = render(header_line,col,scratch,rows,width);
      col += width;
    }
  }
  if(col >= COLS)
    goto done;

  {  // opus plc
    char scratch [LINES][COLS];
    memset(scratch,0,sizeof scratch);
    int session = First_session;
    int width = 10;
    int rows = 0;
    bool enable = false;

    snprintf(scratch[rows++],COLS,"%*s",width,"plc");
    for(;rows < LINES && session < NSESSIONS; rows++,session++){
      struct session const *sp = Sess_ptr[session];
      if(!inuse(sp)) break;
      snprintf(scratch[rows],COLS,"%*llu",width,(unsigned long long)sp->plcs);
      if(sp->plcs > 0)
	enable = true;
    }
    if(enable){
      col++;
      width = render(header_line,col,scratch,rows,width);
      col += width;
    }
  }
  if(col >= COLS)
    goto done;

  {  // sockets
    char scratch [LINES][COLS];
    memset(scratch,0,sizeof scratch);
    int session = First_session;
    int width = COLS;
    int rows = 0;

    snprintf(scratch[rows++],COLS,"%s","sockets");
    for(;rows < LINES && session < NSESSIONS; rows++,session++){
      struct session const *sp = Sess_ptr[session];
      if(!inuse(sp)) break;
      snprintf(scratch[rows],COLS,"%s -> %s",formatsock(&sp->sender,true),sp->dest);
    }
    col++;
    width = render_left(header_line,col,scratch,rows,width);
    col += width;
  }
  if(col >= COLS)
    goto done;


 done:;
  // Embolden the active lines
  row = header_line + 1;
  attr_t attrs = 0;
  short pair = 0;
  attr_get(&attrs, &pair, NULL);
  for(int session = First_session; session < NSESSIONS && row < LINES; session++,row++){
    struct session const *sp = Sess_ptr[session];
    if(!inuse(sp))
      break;

    attr_t attr = A_NORMAL;
    if(gps_time_ns() - sp->last_active < BILLION/2){
      // active within the past 500 ms
      attr |= A_BOLD;
    }

    // 1 adjusts for the titles
    // only underscore to just before the socket entry since it's variable length
   int r =  mvchgat(row,col_save,col,attr,pair,NULL);
   (void)r;
   assert(r == OK);
  }
  move(first_line + Current - First_session,col_save); // Cursor on current line
  // End of display writing
}
static void process_keyboard(void){

  int const c = getch(); // Waits for 'update interval' ms if no input
  if(c == EOF)
    return; // No key hit

  // These commands don't require session locking
  bool serviced = true;
  switch(c){
  case 'Q': // quit program
    atomic_store_explicit(&Terminate,true,memory_order_release);
    break;
  case 'v':
    Verbose = !Verbose;
    break;
  case 'C':
    Constant_delay = !Constant_delay;
    break;
  case 'A': // Start all new sessions muted
    Start_muted = !Start_muted;
    break;
  case 'q':
    Quiet_mode = !Quiet_mode;
    break;
  case '\f':  // Screen repaint (formfeed, aka control-L)
    clearok(curscr,TRUE);
    break;
  case 'h': // Help screen
    help = !help;
    break;
  case 's': // Sort sessions by most recently active (or longest active)
    sort_session_active(); // locks Sess_mutex internally
    break;
  case 't': // Sort sessions by most total activity
    sort_session_total();
    break;
  case 'T':
    Auto_sort = !Auto_sort;
    break;
  case KEY_RESIZE:
  case EOF:
    break;
  default:
    serviced = false; // Not handled in this switch(), so fall through and handle in the next with the lock taken
    break;
  }
  if(serviced)
    return;

  serviced = true;


  // Commands manipulating the current session index
  switch(c){
  case KEY_NPAGE:
    if(First_session + Sessions_per_screen < NSESSIONS)
      First_session += Sessions_per_screen;
    break;
  case KEY_PPAGE:
    if(First_session - Sessions_per_screen >= 0)
      First_session -= Sessions_per_screen;
    break;
  case KEY_HOME: // first session
    First_session = 0;
    break;
  case KEY_END: // last session
    {
      while(Current < NSESSIONS-1 && inuse(Sess_ptr[Current]))
	Current++;
      if(Current > First_session + Sessions_per_screen)
	First_session = Current - Sessions_per_screen;
    }
    First_session = max(0,NSESSIONS - Sessions_per_screen);
    break;
  case '\t':
  case KEY_DOWN:
    if(Current >= NSESSIONS-1 || !inuse(Sess_ptr[Current+1]))
      break;
    Current++;
    if(Current >= First_session + Sessions_per_screen)
      First_session++;
    break;
  case KEY_BTAB:
  case KEY_UP:
    if(Current > 0)
      Current--;
    if(Current < First_session)
      First_session--;
    break;
  default:
    serviced = false;
  }
  if(serviced)
    return;


  // Commands operating on all sessions
  // These *probably* don't require locking?
  serviced = true;
  switch(c){
  case 'U': // Unmute all sessions, resetting any that were muted
    for(int i = 0; i < NSESSIONS; i++){
      struct session *sp = Sess_ptr[i];
      if(inuse(sp) && muted(sp)){
	sp->restart = true; // restart playout
	sp->muted = false;
      }
    }
    break;
  case 'M': // Mute all sessions
    for(int i = 0; i < NSESSIONS; i++){
      struct session *sp = Sess_ptr[i];
      if(inuse(sp))
	atomic_store_explicit(&sp->muted,true,memory_order_release);
    }
    break;
  case 'N':
    Notch = true;
    for(int i=0; i < NSESSIONS; i++){
      struct session *sp = Sess_ptr[i];
      if(inuse(sp)){
	sp->notch_enable = true;
      }
    }
    break;
  case 'R': // Reset all sessions
    for(int i=0; i < NSESSIONS;i++){
      struct session *sp = Sess_ptr[i];
      if(inuse(sp))
	sp->restart = true;
    }
    break;
  case 'F':
    Notch = false;
    for(int i=0; i < NSESSIONS; i++){
      struct session *sp = Sess_ptr[i];
      if(inuse(sp))
	sp->notch_enable = false;
    }
    break;
  default:
    serviced = false; // Not handled by this switch
    break;
  }
  if(serviced)
    return;


  // Commands operating on the current session
  // Check validity of current session pointer so individual cases don't have to
  // Do this last
  serviced = true;

  struct session *sp = Sess_ptr[Current];
  if(!inuse(sp)){
    // Current index not valid
    beep();
    return;
  }

  switch(c){
  case 'f': // Turn off tone notching
    sp->notch_enable = false;
    break;
  case 'n':
    Notch = true;
    sp->notch_enable = true;
    break;
  case '=': // If the user doesn't hit the shift key (on a US keyboard) take it as a '+'
  case '+':
    sp->gain *= 1.122018454; // +1 dB
    break;
  case '_': // Underscore is shifted minus
  case '-':
    sp->gain /= 1.122018454; // -1 dB
    break;
  case KEY_LEFT:
    sp->pan = max(sp->pan - .01,-1.0);
    break;
  case KEY_RIGHT:
    sp->pan = min(sp->pan + .01,+1.0);
    break;
  case KEY_SLEFT: // Shifted left - decrease playout buffer 1 ms
    if(Playout >= -0.1){
      Playout -= .001;
    }
    break;
  case KEY_SRIGHT: // Shifted right - increase playout buffer 1 ms
    Playout += .001;
    break;
  case 'u': // Unmute and reset Current session
    atomic_store_explicit(&sp->muted,false,memory_order_release);
    break;
  case 'm': // Mute Current session
    atomic_store_explicit(&sp->muted,true,memory_order_release);
    break;
  case 'r':    // Manually reset playout queue
    sp->restart = true;
    break;
  case KEY_DC: // Delete
  case KEY_BACKSPACE:
  case 'd': // Delete current session
    close_session(sp);
    break;
  default:
    serviced = false;
    break;
  }
  if(!serviced)
    beep(); // Not serviced by anything
}
static int render(int const row, int const col, char const scratch[LINES][COLS],int const nrows,int const ncols){
  int firstcol = -1;
  for(int i = 0; i < ncols; i++){
    for(int j = 0; j < nrows; j++){
      if(scratch[j][i] != ' ' && scratch[j][i] != '\0'){
	firstcol = i;
	break;
      }
    }
    if(firstcol != -1)
      break;
  }
  int nc = 0;
  if(firstcol != -1){
    nc = ncols - firstcol;

    for(int i = 0; i < nrows; i++){
      int ncol = strlen(scratch[i] + firstcol);
      if(ncol > nc)
	ncol = nc;
      mvaddnstr(row+i,col,scratch[i] + firstcol,ncol);
    }
  }
  return nc;
}
static int render_left(int const row, int const col, char const scratch[LINES][COLS],int const nrows,int const ncols){
  unsigned int maxline = 0;
  for(int j = 0; j < nrows; j++){
    mvaddnstr(row+j,col,scratch[j],ncols);
    if(strlen(scratch[j]) > maxline)
      maxline = strlen(scratch[j]);
  }
  return maxline;
}
