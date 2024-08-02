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
  double freq;
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
static int Current = -1; // No current session
static bool help = false;

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
  int space = COLS - x - 1; // Leave last column open
  if(strlen(str) <= space)
    return mvaddstr(y,x,str); // fits
  char temp[space+1];
  strlcpy(temp,str,sizeof(temp)); // truncate
  return mvaddstr(y,x,temp);
}
int addstrt(char const *str){
  int y,x;
  getyx(stdscr,y,x);
  int space = COLS - x - 1; // Leave last column open
  if(strlen(str) <= space)
    return mvaddstr(y,x,str); // fits
  char temp[space+1];
  strlcpy(temp,str,sizeof(temp)); // truncate
  return mvaddstr(y,x,temp);
}


// Use ncurses to display streams
void *display(void *arg){

  pthread_setname("display");

  if(initscr() == NULL){
    fprintf(stderr,"initscr() failed, disabling control/display thread\n");
    pthread_exit(NULL);
  }
  keypad(stdscr,TRUE);
  timeout(Update_interval);
  cbreak();
  noecho();

  while(!Terminate){
    assert(First_session >= 0);
    assert(First_session == 0 || First_session < Nsessions);
    assert(Current >= -1);
    assert(Current == -1 || Current < Nsessions); // in case Nsessions is 0

    // Start screen update
    move(0,0);
    clrtobot();
    addstrt("KA9Q Multicast Audio Monitor:");
    for(int i=0;i<Nfds;i++)
      printwt(" %s",Mcast_address_text[i]);
    addstrt("\n");

    if(help){
      char path [PATH_MAX];
      dist_path(path,sizeof(path),"monitor-help.txt");
      FILE *fp = fopen(path,"r");
      if(fp != NULL){
	size_t size = 1024;
	char *line = malloc(size);
	while(getline(&line,&size,fp) != -1)
	  addstrt(line);

	FREE(line);
	fclose(fp);
	fp = NULL;
      }
    }
    if(Nsessions == 0)
      Current = -1;
    if(Nsessions > 0 && Current == -1)
      Current = 0;

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

  vote(); // update now_active flags
  if(s1->now_active){
    if(s2->now_active){
      // Both active. Fuzz needed because active sessions are updated when packets arrive
      if(fabsf(s1->active - s2->active) < 0.5) {
	return 0; // Equal within 1/2 sec
      } else if(s1->active > s2->active){
	return -1; // s1 Longer active
      } else {
	return +1; // s2 longer
      }
    } else
      return -1; // s1 active, s2 inactive. Active always lower than inactive
  } else {    // s1 inctive
    if(s2->now_active){
      return +1;
    } else {     // Both inactive, sort by last active times
      if(s1->last_active > s2->last_active){
	return -1;
      } else {
	return +1;
      }
      // last_active is in nanoseconds so chances of equality are nil
    }
  }
}

// sort callback for sort_session() for comparing sessions by total time
static int tcompare(void const *a, void const *b){
  struct session const * const s1 = *(struct session **)a;
  struct session const * const s2 = *(struct session **)b;

#define FUZZ 1
#ifdef FUZZ
  if(fabsf(s1->tot_active - s2->tot_active) < 0.1) // equal within margin
    return 0;
#endif
  if(s1->tot_active > s2->tot_active)
    return -1;
  return +1;
}

// Sort session list in increasing order of age
static int sort_session_active(void){
  qsort(Sessions,Nsessions,sizeof(Sessions[0]),scompare);
  return 0;
}
static int sort_session_total(void){
  qsort(Sessions,Nsessions,sizeof(Sessions[0]),tcompare);
  return 0;
}

void load_id(void){
  char filename[PATH_MAX];
  dist_path(filename,sizeof(filename),ID);
  struct stat statbuf;
  stat(filename,&statbuf);
  if(statbuf.st_mtime != Last_stat.st_mtime)
    Nid = 0; // Force reload

  if(Nid == 0){
    // Load table
    FILE * const fp = fopen(filename,"r");
    if(fp == NULL)
      return;

    char line[1024];
    while(fgets(line,sizeof(line),fp)){
      chomp(line);

      if(line[0] == '#' || strlen(line) == 0)
	continue; // Comment
      assert(Nid < IDSIZE);
      char *ptr = NULL;
      Idtable[Nid].freq = strtod(line,&ptr);
      if(ptr == line)
	continue; // no parseable number

      while(*ptr == ' ' || *ptr == '\t')
	ptr++;
      int const len = strlen(ptr); // Length of ID field
      if(len > 0){ // Not null
	strlcpy(Idtable[Nid].id,ptr,sizeof(Idtable[Nid].id));
      }
      Nid++;
      if(Nid == IDSIZE){
	fprintf(stderr,"ID table overlow, size %d\n",Nid);
	break;
      }
    }
    fclose(fp);
  }
}

char const *lookupid(double freq){
  for(int i=0; i < Nid; i++){
    if(Idtable[i].freq == freq)
      return Idtable[i].id;
  }
  return NULL;
}

static void update_monitor_display(void){
  // First header line
  if(Repeater_tail != 0){
    if(Last_id_time != 0)
      printwt("Last ID: %lld sec",(long long)((gps_time_ns() - Last_id_time) / BILLION));
    if(PTT_state)
      addstrt(" PTT On");
    else if(Last_xmit_time != 0)
      printwt(" PTT Off; Last xmit: %lld sec",(long long)((gps_time_ns() - Last_xmit_time) / BILLION));
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
  
  if(Auto_sort)
    sort_session_active();
  
  Sessions_per_screen = LINES - getcury(stdscr) - 1;
  
  vote(); // update active session flags
  // This mutex protects Sessions[] and Nsessions. Instead of holding the
  // lock for the entire display loop, we make a copy.
  pthread_mutex_lock(&Sess_mutex);
  assert(Nsessions <= NSESSIONS);
  int Nsessions_copy = Nsessions;
  struct session *Sessions_copy[NSESSIONS];
  memcpy(Sessions_copy,Sessions,Nsessions * sizeof(Sessions_copy[0]));
  pthread_mutex_unlock(&Sess_mutex);
  
  if(Verbose){
    // Measure skew between sampling clock and UNIX real time (hopefully NTP synched)
    double pa_seconds = Pa_GetStreamTime(Pa_Stream) - Start_pa_time;
    int q = modsub(Wptr,Rptr,BUFFERSIZE);
    double qd = (double) q / DAC_samprate;
    double rate = Audio_frames / pa_seconds;
    
    printwt("Playout %.0f ms, latency %d ms, queue %.3lf sec, D/A rate %'.3lf Hz,",Playout,Portaudio_delay,qd,rate);
    printwt(" (%+.3lf ppm),",1e6 * (rate / DAC_samprate - 1));
    // Time since last packet drop on any channel
    printwt(" Error-free sec %'.1lf\n",(1e-9*(gps_time_ns() - Last_error_time)));
  }
  // Show channel statuses
  getyx(stdscr,y,x);
  int row_save = y;
  int col_save = x;
  
  // dB column
  int width = 4;
  mvprintwt(y++,x,"%*s",width,"dB");
  for(int session = First_session; session < Nsessions_copy; session++,y++){
    struct session const *sp = Sessions_copy[session];
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
    for(int session = First_session; session < Nsessions_copy; session++,y++){
      struct session const *sp = Sessions_copy[session];
      mvprintwt(y,x,"%*d",width,(int)roundf(100*sp->pan));
    }
    x += width;
    y = row_save;
  }
  // SSRC
  if(x >= COLS)
    goto done;

  width = 9;
  mvprintwt(y++,x,"%*s",width,"ssrc");
  for(int session = First_session; session < Nsessions_copy; session++,y++){
    struct session const *sp = Sessions_copy[session];
    mvprintwt(y,x,"%*d",width,sp->ssrc);
  }
  x += width;
  y = row_save;

  if(Notch){
    if(x >= COLS)
      goto done;
    width = 6;
    mvprintwt(y++,x,"%*s",width,"tone");
    for(int session = First_session; session < Nsessions_copy; session++,y++){
      struct session const *sp = Sessions_copy[session];
      if(!sp->notch_enable || sp->notch_tone == 0)
	continue;
      
      mvprintwt(y,x,"%*.1f%c",width,sp->notch_tone,sp->current_tone == sp->notch_tone ? '*' : ' ');
    }
    x += width;
    y = row_save;
  }
  if(x >= COLS)
    goto done;
  width = 12;
  mvprintwt(y++,x,"%*s",width,"freq");
  for(int session = First_session; session < Nsessions_copy; session++,y++){
    struct session const *sp = Sessions_copy[session];
    mvprintwt(y,x,"%'*.0lf",width,sp->chan.tune.freq);
  }
  x += width;
  y = row_save;

  if(x >= COLS)
    goto done;
  width = 5;
  mvprintwt(y++,x,"%*s",width,"mode");
  for(int session = First_session; session < Nsessions_copy; session++,y++){
    struct session const *sp = Sessions_copy[session];
    mvprintwt(y,x,"%*s",width,sp->chan.preset);
  }
  x += width;
  y = row_save;

  if(x >= COLS)
    goto done;
  width = 5;
  mvprintwt(y++,x,"%*s",width,"s/n");
  for(int session = First_session; session < Nsessions_copy; session++,y++){
    struct session const *sp = Sessions_copy[session];
    if(!isnan(sp->snr))
      mvprintwt(y,x,"%*.1f",width,sp->snr);
  }
  x += width;
  y = row_save;

  x++; // ID is left justified, add a leading space

  if(x >= COLS)
    goto done;
  width = 0;
  mvprintwt(y++,x,"%s","id");
  for(int session = First_session; session < Nsessions_copy; session++,y++){
    struct session const *sp = Sessions_copy[session];
    int len = strlen(sp->id);
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
  for(int session = First_session; session < Nsessions_copy; session++,y++){
    struct session const *sp = Sessions_copy[session];
    char total_buf[100];
    mvprintwt(y,x,"%*s",width,ftime(total_buf,sizeof(total_buf),sp->tot_active));
  }
  x += width;
  y = row_save;

  if(x >= COLS)
    goto done;
  width = 10;
  mvprintwt(y++,x,"%*s",width,"cur/idle");
  {
    long long time = gps_time_ns();
    for(int session = First_session; session < Nsessions_copy; session++,y++){
      struct session *sp = Sessions_copy[session];
      char buf[100];
      if(sp->now_active)
	mvprintwt(y,x,"%*s",width,ftime(buf,sizeof(buf),sp->active));
      else {
	float idle_sec = (time - sp->last_active) / BILLION;
	mvprintwt(y,x,"%*s",width,ftime(buf,sizeof(buf),idle_sec));   // Time idle since last transmission
      }
    }
  }
  x += width;
  y = row_save;

  if(x >= COLS)
    goto done;
  width = 6;
  mvprintwt(y++,x,"%*s",width,"queue");
  for(int session = First_session; session < Nsessions_copy; session++,y++){
    struct session const *sp = Sessions_copy[session];
    if(!sp->now_active || sp->muted || (Voting && Best_session != NULL && Best_session != sp))
      continue;
    
    int const d = modsub(sp->wptr,Rptr,BUFFERSIZE); // Unplayed samples on queue
    int const queue_ms = d > 0 ? 1000 * d / DAC_samprate : 0; // milliseconds
    mvprintwt(y,x,"%*d",width,queue_ms);   // Time idle since last transmission
  }
  x += width;
  y = row_save;

  // Opus/pcm
  x++; // Left justified, add a space
  if(x >= COLS)
    goto done;
  
  width = 6;
  mvprintwt(y++,x,"%-*s",width,"type");
  for(int session = First_session; session < Nsessions_copy; session++,y++){
    struct session const *sp = Sessions_copy[session];
    mvprintwt(y,x,"%-*s",width,encoding_string(PT_table[sp->type].encoding));
  }
  x += width;
  y = row_save;

  // frame size, ms
  if(x >= COLS)
    goto done;
  width = 3;
  mvprintwt(y++,x,"%*s",width,"ms");
  for(int session = First_session; session < Nsessions_copy; session++,y++){
    struct session const *sp = Sessions_copy[session];
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
  for(int session = First_session; session < Nsessions_copy; session++,y++){
    struct session const *sp = Sessions_copy[session];
    mvprintwt(y,x,"%*d",width,sp->channels);
  }
  x += width;
  y = row_save;

  // BW
  if(x >= COLS)
    goto done;
  width = 3;
  mvprintwt(y++,x,"%*s",width,"bw");
  for(int session = First_session; session < Nsessions_copy; session++,y++){
    struct session const *sp = Sessions_copy[session];
    mvprintwt(y,x,"%*d",width,sp->bandwidth);
  }
  x += width;
  y = row_save;

  // RTP payload type
  if(x >= COLS)
    goto done;
  width = 4;
  mvprintwt(y++,x,"%*s",width,"pt");
  for(int session = First_session; session < Nsessions_copy; session++,y++){
    struct session const *sp = Sessions_copy[session];
    mvprintwt(y,x,"%*d",width,sp->type);
  }
  x += width;
  y = row_save;

  // Packets
  if(x >= COLS)
    goto done;
  width = 12;
  mvprintwt(y++,x,"%*s",width,"packets");
  for(int session = First_session; session < Nsessions_copy; session++,y++){
    struct session const *sp = Sessions_copy[session];
    mvprintwt(y,x,"%*lu",width,sp->packets);
  }
  x += width;
  y = row_save;

  // Resets
  if(x >= COLS)
    goto done;
  width = 7;
  mvprintwt(y++,x,"%*s",width,"resets");
  for(int session = First_session; session < Nsessions_copy; session++,y++){
    struct session const *sp = Sessions_copy[session];
    mvprintwt(y,x,"%*lu",width,sp->resets);
  }
  x += width;
  y = row_save;

  // BW
  if(x >= COLS)
    goto done;
  width = 6;
  mvprintwt(y++,x,"%*s",width,"drops");
  for(int session = First_session; session < Nsessions_copy; session++,y++){
    struct session const *sp = Sessions_copy[session];
    mvprintwt(y,x,"%'*llu",width,(unsigned long long)sp->rtp_state.drops);
  }
  x += width;
  y = row_save;

  // Lates
  if(x >= COLS)
    goto done;
  width = 6;
  mvprintwt(y++,x,"%*s",width,"lates");
  for(int session = First_session; session < Nsessions_copy; session++,y++){
    struct session const *sp = Sessions_copy[session];
    mvprintwt(y,x,"%*lu",width,sp->lates);
  }
  x += width;
  y = row_save;

  // BW
  if(x >= COLS)
    goto done;
  width = 6;
  mvprintwt(y++,x,"%*s",width,"reseq");
  for(int session = First_session; session < Nsessions_copy; session++,y++){
    struct session const *sp = Sessions_copy[session];
    mvprintwt(y,x,"%*lu",width,sp->reseqs);
  }
  x += width;
  y = row_save;

  // Sockets
  x++; // Left justified
  mvprintwt(y++,x,"%s","sockets");
  for(int session = First_session; session < Nsessions_copy; session++,y++){
    struct session const *sp = Sessions_copy[session];
    mvprintwt(y,x,"%s -> %s",formatsock(&sp->sender),sp->dest);
  }
 done:;
  // Embolden the active lines
  attr_t attrs = 0;
  short pair = 0;
  attr_get(&attrs, &pair, NULL);
  for(int session = First_session; session < Nsessions_copy; session++){
    struct session const *sp = Sessions_copy[session];
    
    attr_t attr = A_NORMAL;
    if(sp->now_active)
      attr |= A_BOLD;
    
    // 1 adjusts for the titles
    // only underscore to just before the socket entry since it's variable length
    mvchgat(1 + row_save + session,col_save,x,attr,pair,NULL);
  }
  if(Current != -1)
    move(1 + row_save + Current,col_save); // Cursor on current line
  // End of display writing
}
static void process_keyboard(void){

  int const c = getch(); // Waits for 'update interval' ms if no input
  if(c == EOF)
    return; // No key hit
  
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
  case 'U': // Unmute all sessions, resetting any that were muted
    for(int i = 0; i < Nsessions; i++){
      struct session *sp = sptr(i);
      if(sp && sp->muted){
	sp->reset = true; // Resynchronize playout buffer (output callback may have paused)
	sp->muted = false;
      }
    }
    break;
  case 'M': // Mute all sessions
    for(int i = 0; i < Nsessions; i++){
      struct session *sp = sptr(i);
      if(sp)
	sp->muted = true;
    }
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
    sort_session_active();
    break;
  case 'S':
    Auto_sort = !Auto_sort;
    break;
  case 't': // Sort sessions by most recently active (or longest active)
    sort_session_total();
    break;
  case 'N':
    Notch = true;
    for(int i=0; i < Nsessions; i++){
      struct session *sp = sptr(i);
      if(sp != NULL){
	sp->notch_enable = true;
      }
    }
    break;
  case 'n':
    Notch = true;
    if(Current >= 0){
      struct session *sp = sptr(Current);
      if(sp)
	sp->notch_enable = true;
    }
    break;
  case 'R': // Reset all sessions
    for(int i=0; i < Nsessions;i++){
      struct session *sp = sptr(i);
      if(sp)
	sp->reset = true;
    }
    break;
  case 'f': // Turn off tone notching
    if(Current >= 0){
      struct session *sp = sptr(Current);
      if(sp)
	sp->notch_enable = false;
    }
    break;
  case 'F':
    Notch = false;
    for(int i=0; i < Nsessions; i++){
      struct session *sp = sptr(i);
      if(sp)
	sp->notch_enable = false;
    }
    break;
  case KEY_RESIZE:
  case EOF:
    break;
  case KEY_NPAGE:
    if(First_session + Sessions_per_screen < Nsessions){
      First_session += Sessions_per_screen;
      Current += Sessions_per_screen;
      if(Current > Nsessions-1)
	Current = Nsessions - 1;
    }
    break;
  case KEY_PPAGE:
    if(First_session - Sessions_per_screen >= 0){
      First_session -= Sessions_per_screen;
      Current -= Sessions_per_screen;
    }
    break;
  case KEY_HOME: // first session
    if(Nsessions > 0){
      Current = 0;
      First_session = 0;
    }
    break;
  case KEY_END: // last session
    if(Nsessions > 0){
      Current = Nsessions-1;
      First_session = max(0,Nsessions - Sessions_per_screen);
    }
    break;
  case '\t':
  case KEY_DOWN:
    if(Current >= 0 && Current < Nsessions-1){
      Current++;
      if(Current >= First_session + Sessions_per_screen - 1)
	First_session++;
    }
    break;
  case KEY_BTAB:
  case KEY_UP:
    if(Current > 0){
      Current--;
      if(Current < First_session)
	First_session--;
    }
    break;
  case '=': // If the user doesn't hit the shift key (on a US keyboard) take it as a '+'
  case '+':
    {
      struct session *sp = sptr(Current);
      if(sp)
	sp->gain *= 1.122018454; // +1 dB
    }
    break;
  case '_': // Underscore is shifted minus
  case '-':
    {
      struct session *sp = sptr(Current);
      if(sp)
	sp->gain /= 1.122018454; // -1 dB
    }
    break;
  case KEY_LEFT:
    {
      struct session *sp = sptr(Current);
      if(sp)
	sp->pan = max(sp->pan - .01,-1.0);
    }
    break;
  case KEY_RIGHT:
    {
      struct session *sp = sptr(Current);
      if(sp)
	sp->pan = min(sp->pan + .01,+1.0);
    }
    break;
  case KEY_SLEFT: // Shifted left - decrease playout buffer 10 ms
    if(Playout >= -100){
      Playout -= 1;
      struct session *sp = sptr(Current);
      if(sp)
	sp->reset = true;
    }
    break;
  case KEY_SRIGHT: // Shifted right - increase playout buffer 10 ms
    Playout += 1;
    {
      struct session *sp = sptr(Current);
      if(sp)
	sp->reset = true;
      else
	beep();
    }
    break;
  case 'u': // Unmute and reset Current session
    {
      struct session *sp = sptr(Current);
      if(sp && sp->muted){
	sp->reset = true; // Resynchronize playout buffer (output callback may have paused)
	sp->muted = false;
      }
    }
    break;
  case 'm': // Mute Current session
    {
      struct session *sp = sptr(Current);
      if(sp)
	sp->muted = true;
    }
    break;
  case 'r':
    // Manually reset playout queue
    {
      struct session *sp = sptr(Current);
      if(sp)
	sp->reset = true;
    }
    break;
  case KEY_DC: // Delete
  case KEY_BACKSPACE:
  case 'd': // Delete current session
    {
      struct session *sp = sptr(Current);
      if(sp){
	sp->terminate = true;
	// We have to wait for it to clean up before we close and remove its session
	pthread_join(sp->task,NULL);
	close_session(&sp); // Decrements Nsessions
      }
      if(Current >= Nsessions)
	Current = Nsessions-1; // -1 when no sessions
    }
    break;
  default: // Invalid command
    beep();
    break;
  }
}
