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

static int First_session = 0;
static int Sessions_per_screen = 0;
static int Current = 0;
static bool help = false;

struct session *Sess_ptr[NSESSIONS];

// Versions of ncurses routines that truncate at EOL
// Remaining problem: if I use the last column, the position will still
// wrap to the first column of the next row, and I can't stop that except by not using the last column.

int mvprintwt(int y,int x,char const *fmt,...){
  if(x < 0)
    return ERR;

  int space = COLS - x - 1; // leave last column open
  if(space <= 0)
    return ERR;

  va_list ap;
  va_start(ap,fmt);
  char string[COLS+1]; // Maximum line length plus null
  int r = vsnprintf(string,space,fmt,ap); // write only 'space' characters, space <= COLS
  va_end(ap);
  if(r < 0)
    return ERR;
  mvaddstr(y,x,string);
  return OK;
}

// Same for printw() - truncate at EOL
int printwt(char const *fmt,...){
  int y,x;
  getyx(stdscr,y,x);
  if(x < 0)
    return ERR;

  int space = COLS - x - 1; // leave last column open
  if(space <= 0)
    return ERR;

  va_list ap;
  va_start(ap,fmt);
  char string[COLS+1]; // Maximum line length plus null
  int r = vsnprintf(string,space,fmt,ap); // write only 'space' characters, space <= COLS
  va_end(ap);
  if(r < 0)
    return ERR;
  mvaddstr(y,x,string);
  return OK;
}

// Same for mvaddstr() and addstr()
int mvaddstrt(int y,int x,char const *str){
  size_t space = COLS - x - 1; // Leave last column open
  if(strlen(str) <= space)
    return mvaddstr(y,x,str); // fits
  char temp[space+1];
  strlcpy(temp,str,sizeof(temp)); // truncate
  return mvaddstr(y,x,temp);
}
int addstrt(char const *str){
  int y,x;
  getyx(stdscr,y,x);
  size_t space = COLS - x - 1; // Leave last column open
  if(strlen(str) <= space)
    return mvaddstr(y,x,str); // fits
  char temp[space+1];
  strlcpy(temp,str,sizeof(temp)); // truncate
  return mvaddstr(y,x,temp);
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

  while(!Terminate){
    // Start screen update
    move(0,0);
    clrtobot();
    if(Source == NULL)
      addstrt("KA9Q Multicast Audio Monitor:");
    else
      printwt("KA9Q Multicast Audio Monitor, only from %s:",Source);

    for(int i=0;i<Nfds;i++)
      printwt(" %s",Mcast_address_text[i]);
    addstrt("\n");

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
    vote(); // mainly in case a session was muted or unmuted
  }
  return NULL;
}
// sort callback for sort_session_active() for comparing sessions by most recently active (or currently longest active)
static int scompare(void const *a, void const *b){
  struct session const * const s1 = *(struct session **)a;
  struct session const * const s2 = *(struct session **)b;

  if(s1 == s2) // same session compares equal (shouldn't happen)
    return 0;
  if(s1 == NULL || !s1->inuse) // first is empty or idle, put it at end
    return +1;
  if(s2 == NULL || !s2->inuse) // second is empty or idle, put it at end
    return -1;

  if(s1->running){
    if(s2->running){
      // Both active. Fuzz needed because active sessions are updated when packets arrive
      if(fabs(s1->active - s2->active) < 0.5) {
	return s1->ssrc > s2->ssrc ? +1 : -1; // resolve ties by ssrc to stop rapid flipping
      } else if(s1->active > s2->active){
	return -1; // s1 Longer active
      } else {
	return +1; // s2 longer
      }
    } else
      return -1; // s1 active, s2 inactive. Active always lower than inactive
  } else {    // s1 inctive
    if(s2->running){
      return +1;
    } else {     // Both inactive, sort by last active times
      return s1->last_active > s2->last_active ? -1 : +1;
      // last_active is in nanoseconds so chances of equality are nil
    }
  }
}

// sort callback for sort_session() for comparing sessions by total time
static int tcompare(void const *a, void const *b){
  struct session const * const s1 = *(struct session **)a;
  struct session const * const s2 = *(struct session **)b;

  if(s1 == s2) // same session compares equal (shouldn't happen)
    return 0;
  if(s1 == NULL || !s1->inuse) // first is empty or idle, put it at end
    return +1;
  if(s2 == NULL || !s2->inuse) // second is empty or idle, put it at end
    return -1;

#define FUZZ 1
#ifdef FUZZ
  if(fabs(s1->tot_active - s2->tot_active) < 0.5) // equal within margin
    return s1->ssrc > s2->ssrc ? +1 : -1; // resolve ties by ssrc to stop rapid flipping
#endif
  if(s1->tot_active > s2->tot_active)
    return -1;
  return +1;
}

// Defragment list
static int defragment_session(void){
  pthread_mutex_lock(&Sess_mutex);
  for(int i=0; i < NSESSIONS-1; i++){
    // Find next unused slot
    if(Sess_ptr[i]->inuse)
      continue;

    // Find following used slot
    int j = i+1;
    while(j < NSESSIONS && !Sess_ptr[j]->inuse)
      j++;

    if(j == NSESSIONS)
      break;
    struct session *save = Sess_ptr[i];
    Sess_ptr[i] = Sess_ptr[j];
    Sess_ptr[j] = save;
    i = j; // resume after this one
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
  // First header line
  if(Repeater_tail != 0){
    if(Last_id_time != 0)
      printwt("Last ID: %.1lf sec",((double)(gps_time_ns() - Last_id_time) * 1e-9));
    if(PTT_state)
      addstrt(" PTT On");
    else if(Last_xmit_time != 0)
      printwt(" PTT Off; Last xmit: %.1lf sec",(double)(gps_time_ns() - Last_xmit_time) * 1e-9);
    printwt("\n");
  }
  if(Constant_delay)
    printwt("Constant delay ");

  if(Start_muted)
    printwt("**Starting new sessions muted** ");

  if(Voting)
    printwt("SNR Voting enabled\n");

  int y,x;
  getyx(stdscr,y,x);
  if(x != 0)
    printwt("\n");

  if(Verbose){
    // Measure skew between sampling clock and UNIX real time (hopefully NTP synched)
    double const pa_seconds = Pa_GetStreamTime(Pa_Stream) - Start_pa_time;

    uint64_t audio_frames = atomic_load_explicit(&Audio_frames,memory_order_relaxed);
    double const rate = audio_frames / pa_seconds;
    int64_t rptr = atomic_load_explicit(&Output_time,memory_order_relaxed);

    printwt("%s playout %.0lf ms latency %5.1lf ms D/A %'.1lf Hz,",
	    opus_get_version_string(),1000*Playout,1000*Portaudio_delay,rate);
    printwt(" (%+4.0lf ppm),",1e6 * (rate / DAC_samprate - 1));
    // Time since last packet drop on any channel
    printwt(" EFS %'.1lf",(1e-9*(gps_time_ns() - Last_error_time)));
    //    int64_t total = atomic_load(&Output_total);
    //    int64_t calls = atomic_load(&Callbacks);
    int quant = atomic_load_explicit(&Callback_quantum,memory_order_relaxed);
    double level = atomic_load_explicit(&Output_level,memory_order_relaxed);
    level = power2dB(level);
    printwt(" Clock %.1lfs %.1lf dBFS CB N %u",(double)rptr/DAC_samprate,level,quant);
    printwt("\n");
  }
  Sessions_per_screen = LINES - getcury(stdscr) - 1;

  defragment_session();
  if(Auto_sort)
    sort_session_total();

  if(First_session >= NSESSIONS)
    First_session = NSESSIONS-1;
  while(First_session > 0 && !Sess_ptr[First_session]->inuse)
    First_session--;

  // Show channel statuses
  getyx(stdscr,y,x);
  int row_save = y;
  int col_save = x;
  int first_line = row_save+1; // after header line

  // Bound current pointer to active list area
  while(Current >= 0 && !Sess_ptr[Current]->inuse)
    Current--; // Current session is no valid, back up

  if(Current <= First_session)
    Current = First_session;
  else if(Current >= LINES - first_line)
    Current = LINES - first_line;




  // dB column
  int width = 4;
  mvprintwt(y++,x,"%*s",width,"dB");
  for(int session = First_session; session < NSESSIONS &&  y < LINES; session++,y++){
    struct session const *sp = Sess_ptr[session];
    if(!sp->inuse) break;
    mvprintwt(y,x,"%+*.0lf",width,sp->muted ? -INFINITY : voltage2dB(sp->gain));
  }
  x += width;
  y = row_save;
  if(Auto_position){
    if(x >= COLS)
      goto done;

    // Pan column
    width = 4;
    mvprintwt(y++,x," pan");
    for(int session = First_session; session < NSESSIONS && y < LINES; session++,y++){
      struct session const *sp = Sess_ptr[session];
      if(!sp->inuse) break;
      mvprintwt(y,x,"%*d",width,(int)round(100*sp->pan));
    }
    x += width;
    y = row_save;
  }
  // SSRC
  if(x >= COLS)
    goto done;

  width = 9;
  mvprintwt(y++,x,"%*s",width,"ssrc");
  for(int session = First_session; session < NSESSIONS && y < LINES; session++,y++){
    struct session const *sp = Sess_ptr[session];
    if(!sp->inuse) break;
    mvprintwt(y,x,"%*d",width,sp->ssrc);
  }
  x += width;
  y = row_save;

  if(Notch){
    if(x >= COLS)
      goto done;
    width = 7;
    mvprintwt(y++,x,"%*s",width,"tone");
    for(int session = First_session; session < NSESSIONS && y < LINES; session++,y++){
      struct session const *sp = Sess_ptr[session];
      if(!sp->inuse)
	break;
      if(sp->notch_enable && sp->notch_tone != 0)
	mvprintwt(y,x,"%*.1f%c",width-1,sp->notch_tone,sp->current_tone == sp->notch_tone ? '*' : ' ');

    }
    x += width;
    y = row_save;
  }
  if(x >= COLS)
    goto done;
  width = 12;
  mvprintwt(y++,x,"%*s",width,"freq");
  for(int session = First_session; session < NSESSIONS && y < LINES; session++,y++){
    struct session const *sp = Sess_ptr[session];
    if(!sp->inuse) break;
      mvprintwt(y,x,"%'*.0lf",width,sp->chan.tune.freq);
  }
  x += width;
  y = row_save;

  if(x >= COLS)
    goto done;
  width = 5;
  mvprintwt(y++,x,"%*s",width,"mode");
  for(int session = First_session; session < NSESSIONS && y < LINES; session++,y++){
    struct session const *sp = Sess_ptr[session];
    if(!sp->inuse) break;
      mvprintwt(y,x,"%*s",width,sp->chan.preset);
  }
  x += width;
  y = row_save;

  if(x >= COLS)
    goto done;
  width = 6;
  mvprintwt(y++,x,"%*s",width-1,"s/n");
  for(int session = First_session; session < NSESSIONS && y < LINES; session++,y++){
    struct session const *sp = Sess_ptr[session];
    if(sp->inuse && !isnan(sp->snr))
      mvprintwt(y,x,"%*.1f%c",width-1,sp->snr,(Voting && sp == Best_session) ? '*' : ' ');
  }
  x += width;
  y = row_save;

  x++; // ID is left justified, add a leading space
  if(x >= COLS)
    goto done;
  width = 0;
  mvprintwt(y++,x,"%s","id");
  for(int session = First_session; session < NSESSIONS && y < LINES; session++,y++){
    struct session const *sp = Sess_ptr[session];
    if(!sp->inuse)
      continue;
    int len = (int)strlen(sp->id);
    if(len > width)
      width = len;
    mvprintwt(y,x,"%s",sp->id);
  }
  x += width;
  y = row_save;

  if(x >= COLS)
    goto done;
  width = 10;
  mvprintwt(y++,x,"%*s",width,"total");
  for(int session = First_session; session < NSESSIONS && y < LINES; session++,y++){
    struct session const *sp = Sess_ptr[session];
    if(!sp->inuse)
      continue;
    char total_buf[100];
    mvprintwt(y,x,"%*s",width,ftime(total_buf,sizeof(total_buf),(int64_t)round(sp->tot_active)));
  }
  x += width;
  y = row_save;

  if(x >= COLS)
    goto done;
  width = 10;
  mvprintwt(y++,x,"%*s",width,"cur/idle");
  {
    long long time = gps_time_ns();
    for(int session = First_session; session < NSESSIONS && y < LINES; session++,y++){
      struct session *sp = Sess_ptr[session];
      if(!sp->inuse)
	continue;
      int64_t t = (int64_t)round( sp->running ? sp->active : (time - sp->last_active) * 1e-9);
      char buf[100];
      mvprintwt(y,x,"%*s",width,ftime(buf,sizeof(buf),t));
    }
  }
  x += width;
  y = row_save;

  if(x >= COLS)
    goto done;
  width = 6;
  mvprintwt(y++,x,"%*s",width,"level");
  for(int session = First_session; session < NSESSIONS && y < LINES; session++,y++){
    struct session const *sp = Sess_ptr[session];
    if(!sp->inuse)
      continue;

    double dB = power2dB(sp->level);
    if(dB >= -99)
      mvprintwt(y,x,"%*.1lf",width,dB);   // Time idle since last transmission

  }
  x += width;
  y = row_save;
  if(x >= COLS)
    goto done;

  width = 6;
  int64_t rptr = atomic_load_explicit(&Output_time,memory_order_relaxed);
  mvprintwt(y++,x,"%*s",width,"queue");
  for(int session = First_session; session < NSESSIONS && y < LINES; session++,y++){
    struct session const *sp = Sess_ptr[session];
    if(!sp->inuse)
      continue;

    int64_t wptr = atomic_load_explicit(&sp->wptr,memory_order_relaxed);
    int64_t d = (wptr - rptr);
    if(d > 0){
      int64_t const queue_ms = 1000 * d / DAC_samprate; // milliseconds
      mvprintwt(y,x,"%*lld",width,queue_ms);   // Time idle since last transmission
    }
  }
  x += width;
  y = row_save;

  // Opus/pcm
  x++; // Left justified, add a space
  if(x >= COLS)
    goto done;

  width = 6;
  mvprintwt(y++,x,"%-*s",width,"type");
  for(int session = First_session; session < NSESSIONS && y < LINES; session++,y++){
    struct session const *sp = Sess_ptr[session];
    if(!sp->inuse) break;
    mvprintwt(y,x,"%-*s",width,encoding_string(sp->pt_table[sp->type].encoding));
  }
  x += width;
  y = row_save;

  // frame size, ms
  if(x >= COLS)
    goto done;
  width = 3;
  mvprintwt(y++,x,"%*s",width,"ms");
  for(int session = First_session; session < NSESSIONS && y < LINES; session++,y++){
    struct session const *sp = Sess_ptr[session];
    if(!sp->inuse) break;
    if(sp->samprate != 0)
      mvprintwt(y,x,"%*d",width,(1000 * sp->frame_size/sp->samprate)); // frame size, ms

  }
  x += width;
  y = row_save;

  // channels
  if(x >= COLS)
    goto done;
  width = 2;
  mvprintwt(y++,x,"%*s",width,"c");
  for(int session = First_session; session < NSESSIONS && y < LINES; session++,y++){
    struct session const *sp = Sess_ptr[session];
    if(!sp->inuse)
      break;
    enum encoding encoding = sp->pt_table[sp->type].encoding;
    if(encoding == OPUS || encoding == OPUS_VOIP)
      mvprintwt(y,x,"%*d",width,sp->opus_channels); // actual number in incoming stream
    else
      mvprintwt(y,x,"%*d",width,sp->channels);

  }
  x += width;
  y = row_save;

  // BW
  if(x >= COLS)
    goto done;
  width = 3;
  mvprintwt(y++,x,"%*s",width,"bw");
  for(int session = First_session; session < NSESSIONS && y < LINES; session++,y++){
    struct session const *sp = Sess_ptr[session];
    if(!sp->inuse) break;
    mvprintwt(y,x,"%*d",width,sp->bandwidth/1000); // convert to kHz
  }
  x += width;
  y = row_save;

  // RTP payload type
  if(x >= COLS)
    goto done;
  width = 4;
  mvprintwt(y++,x,"%*s",width,"pt");
  for(int session = First_session; session < NSESSIONS && y < LINES; session++,y++){
    struct session const *sp = Sess_ptr[session];
    if(!sp->inuse) break;
    mvprintwt(y,x,"%*d",width,sp->type);
  }
  x += width;
  y = row_save;

  // Data rate, kb/s
  if(x >= COLS)
    goto done;
  width = 6;
  mvprintwt(y++,x,"%*s",width,"rate");
  for(int session = First_session; session < NSESSIONS && y < LINES; session++,y++){
    struct session const *sp = Sess_ptr[session];
    if(!sp->inuse) break;
    mvprintwt(y,x,"%*.*f", width, sp->datarate < 1e6 ? 1 : 0, .001 * sp->datarate); // decimal only if < 1000
  }
  x += width;
  y = row_save;

  // Processing delay, assuming synchronized system clocks
  if(x >= COLS)
    goto done;
  width = 8;
  mvprintwt(y++,x,"%*s",width,"Delay");
  for(int session = First_session; session < NSESSIONS && y < LINES; session++,y++){
    struct session const *sp = Sess_ptr[session];
    if(!sp->inuse) break;
    if(sp->chan.output.rtp.timestamp != 0 && sp->running){
      double delay = 0;
      // sp->frontend.timestamp (GPS time at front end) and sp->chan.output.rtp.timestamp (next RTP timestamp to be sent) are updated periodically by status packets
      // sp->rtp_state.timestamp contains most recent RTP packet processed
      // This needs further thought and cleanup
      delay = (double)(int32_t)(sp->chan.output.rtp.timestamp - sp->rtp_state.timestamp) / sp->samprate;
      delay += 1.0e-9 * (gps_time_ns() - sp->chan.clocktime);
      mvprintwt(y,x,"%*.3lf", width, delay);
    }
  }
  x += width;
  y = row_save;

  // Packets
  if(x >= COLS)
    goto done;
  width = 12;
  mvprintwt(y++,x,"%*s",width,"packets");
  for(int session = First_session; session < NSESSIONS && y < LINES; session++,y++){
    struct session const *sp = Sess_ptr[session];
    if(!sp->inuse) break;
    mvprintwt(y,x,"%*lu",width,sp->packets);
  }
  x += width;
  y = row_save;

  // Resets
  if(x >= COLS)
    goto done;
  width = 7;
  mvprintwt(y++,x,"%*s",width,"resets");
  for(int session = First_session; session < NSESSIONS && y < LINES; session++,y++){
    struct session const *sp = Sess_ptr[session];
    if(!sp->inuse) break;
    mvprintwt(y,x,"%*lu",width,sp->resets);
  }
  x += width;
  y = row_save;

  // BW
  if(x >= COLS)
    goto done;
  width = 6;
  mvprintwt(y++,x,"%*s",width,"drops");
  for(int session = First_session; session < NSESSIONS && y < LINES; session++,y++){
    struct session const *sp = Sess_ptr[session];
    if(!sp->inuse) break;
    mvprintwt(y,x,"%'*llu",width,(unsigned long long)sp->rtp_state.drops);
  }
  x += width;
  y = row_save;

  // Lates
  if(x >= COLS)
    goto done;
  width = 6;
  mvprintwt(y++,x,"%*s",width,"lates");
  for(int session = First_session; session < NSESSIONS && y < LINES; session++,y++){
    struct session const *sp = Sess_ptr[session];
    if(!sp->inuse) break;
    mvprintwt(y,x,"%*lu",width,sp->lates);
  }
  x += width;
  y = row_save;

  // BW
  if(x >= COLS)
    goto done;
  width = 6;
  mvprintwt(y++,x,"%*s",width,"reseq");
  for(int session = First_session; session < NSESSIONS && y < LINES; session++,y++){
    struct session const *sp = Sess_ptr[session];
    if(!sp->inuse) break;
    mvprintwt(y,x,"%*lu",width,sp->reseqs);
  }
  x += width;
  y = row_save;

  // Opus loss conceals
  if(x >= COLS)
    goto done;
  width = 7;
  mvprintwt(y++,x,"%*s",width,"PLC");
  for(int session = First_session; session < NSESSIONS && y < LINES; session++,y++){
    struct session const *sp = Sess_ptr[session];
    if(!sp->inuse) break;
    mvprintwt(y,x,"%*lu",width,sp->plcs);
  }
  x += width;
  y = row_save;

  // Sockets
  x++; // Left justified
  mvprintwt(y++,x,"%s","sockets");
  for(int session = First_session; session < NSESSIONS && y < LINES; session++,y++){
    struct session const *sp = Sess_ptr[session];
    if(!sp->inuse) break;
    mvprintwt(y,x,"%s -> %s",formatsock(&sp->sender,true),sp->dest);
  }
 done:;
  // Embolden the active lines
  attr_t attrs = 0;
  short pair = 0;
  attr_get(&attrs, &pair, NULL);
  for(int session = First_session; session < NSESSIONS && y < LINES; session++,y++){
    struct session const *sp = Sess_ptr[session];
    if(!sp->inuse)
      break;

    attr_t attr = A_NORMAL;
    long long const time = gps_time_ns();
    if((time - sp->last_active) < BILLION/2) // active within the past 500 ms
      attr |= A_BOLD;

    // 1 adjusts for the titles
    // only underscore to just before the socket entry since it's variable length
    mvchgat(1 + row_save + session,col_save,x,attr,pair,NULL);
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
    Terminate = true;
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
    First_session = max(0,NSESSIONS - Sessions_per_screen);
    break;
  case '\t':
  case KEY_DOWN: // We'll clip this on the next iteration
    Current++;
    break;
  case KEY_BTAB:
  case KEY_UP:
    Current--;
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
      struct session *sp = Sessions + i;
      if(sp != NULL && sp->muted){
	sp->reset = true; // Resynchronize playout buffer (output callback may have paused)
	sp->muted = false;
      }
    }
    break;
  case 'M': // Mute all sessions
    for(int i = 0; i < NSESSIONS; i++){
      struct session *sp = Sessions + i;
      if(sp != NULL)
	sp->muted = true;
    }
    break;
  case 'N':
    Notch = true;
    for(int i=0; i < NSESSIONS; i++){
      struct session *sp = Sessions + i;
      if(sp != NULL){
	sp->notch_enable = true;
      }
    }
    break;
  case 'R': // Reset all sessions
    for(int i=0; i < NSESSIONS;i++){
      struct session *sp = Sessions + i;
      if(sp != NULL)
	sp->reset = true;
    }
    break;
  case 'F':
    Notch = false;
    for(int i=0; i < NSESSIONS; i++){
      struct session *sp = Sessions + i;
      if(sp != NULL)
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

  struct session *sp = Sessions + Current;
  if(!sp->inuse){
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
      sp->reset = true;
    }
    break;
  case KEY_SRIGHT: // Shifted right - increase playout buffer 1 ms
    Playout += .001;
    sp->reset = true;
    break;
  case 'u': // Unmute and reset Current session
    if(sp->muted){
      sp->reset = true; // Resynchronize playout buffer (output callback may have paused)
      sp->muted = false;
    }
    break;
  case 'm': // Mute Current session
    sp->muted = true;
    break;
  case 'r':    // Manually reset playout queue
    sp->reset = true;
    break;
  case KEY_DC: // Delete
  case KEY_BACKSPACE:
  case 'd': // Delete current session
    close_session(sp);
    pthread_join(sp->task,NULL);
    pthread_t nullthread = {0};
    sp->task = nullthread;
    break;
  default:
    serviced = false;
    break;
  }
  if(!serviced)
    beep(); // Not serviced by anything
}
