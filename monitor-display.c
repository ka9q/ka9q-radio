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
    addstr("KA9Q Multicast Audio Monitor:");
    for(int i=0;i<Nfds;i++)
      printw(" %s",Mcast_address_text[i]);
    addstr("\n");

    if(help){
      char path [PATH_MAX];
      dist_path(path,sizeof(path),"monitor-help.txt");
      FILE *fp = fopen(path,"r");
      if(fp != NULL){
	size_t size = 1024;
	char *line = malloc(size);
	while(getline(&line,&size,fp) != -1)
	  addstr(line);

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
      addstr("Hit 'q' to resume screen updates\n");
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

  if(s1->active > 0 && s2->active > 0){
    // Fuzz needed because active sessions are updated when packets arrive
    if(fabsf(s1->active - s2->active) < 0.5)
      return 0; // Equal within 1/2 sec
    if(s1->active > s2->active)
      return -1; // Longer active lower
    else
      return +1;
  }
  if(s1->active <= 0 && s2->active > 0)
    return +1; // Active always lower than inactive
  if(s1->active >= 0 && s2->active < 0)
    return -1;

  // Both inactive
  if(s1->last_active > s2->last_active)
    return -1;
  else
    return +1;
  // Chances of equality are nil
}
// sort callback for sort_session() for comparing sessions by total time
static int tcompare(void const *a, void const *b){
  struct session const * const s1 = *(struct session **)a;
  struct session const * const s2 = *(struct session **)b;

#if NOFUZZ
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
      printw("Last ID: %lld sec",(long long)((gps_time_ns() - Last_id_time) / BILLION));
    if(PTT_state)
      addstr(" PTT On");
    else if(Last_xmit_time != 0)
      printw(" PTT Off; Last xmit: %lld sec",(long long)((gps_time_ns() - Last_xmit_time) / BILLION));
    printw("\n");
  }
  if(Constant_delay)
    printw("Constant delay ");
  
  if(Start_muted)
    printw("**Starting new sessions muted** ");
  
  if(Voting)
    printw("SNR Voting enabled\n");
  
  int y,x;
  getyx(stdscr,y,x);
  if(x != 0)
    printw("\n");
  
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
    
    printw("Playout %.0f ms, latency %d ms, queue %.3lf sec, D/A rate %'.3lf Hz,",Playout,Portaudio_delay,qd,rate);
    printw(" (%+.3lf ppm),",1e6 * (rate / DAC_samprate - 1));
    // Time since last packet drop on any channel
    printw(" Error-free sec %'.1lf\n",(1e-9*(gps_time_ns() - Last_error_time)));
  }
  // Show channel statuses
  getyx(stdscr,y,x);
  int row_save = y;
  int col_save = x;
  
  // dB column
  mvprintw(y++,x,"%4s","dB");
  for(int session = First_session; session < Nsessions_copy; session++,y++){
    struct session const *sp = Sessions_copy[session];
    mvprintw(y,x,"%+4.0lf",sp->muted ? -INFINITY : voltage2dB(sp->gain));
  }
  x += 5;
  y = row_save;
  if(Auto_position){
    // Pan column
    mvprintw(y++,x," Pan");
    for(int session = First_session; session < Nsessions_copy; session++,y++){
      struct session const *sp = Sessions_copy[session];
      mvprintw(y,x,"%4d",(int)roundf(100*sp->pan));
    }
    x += 4;
    y = row_save;
  }
  
  // SSRC
  mvprintw(y++,x,"%9s","SSRC");
  for(int session = First_session; session < Nsessions_copy; session++,y++){
    struct session const *sp = Sessions_copy[session];
    mvprintw(y,x,"%9d",sp->ssrc);
  }
  x += 10;
  y = row_save;

  if(Notch){
    mvprintw(y++,x,"%5s","Tone");
    for(int session = First_session; session < Nsessions_copy; session++,y++){
      struct session const *sp = Sessions_copy[session];
      if(!sp->notch_enable || sp->notch_tone == 0)
	continue;
      
      mvprintw(y,x,"%5.1f%c",sp->notch_tone,sp->current_tone == sp->notch_tone ? '*' : ' ');
    }
    x += 7;
    y = row_save;
  }
  mvprintw(y++,x,"%12s","Freq");
  for(int session = First_session; session < Nsessions_copy; session++,y++){
    struct session const *sp = Sessions_copy[session];
    mvprintw(y,x,"%'12.0lf",sp->chan.tune.freq);
  }
  x += 13;
  y = row_save;
  
  mvprintw(y++,x,"%5s","Mode");
  for(int session = First_session; session < Nsessions_copy; session++,y++){
    struct session const *sp = Sessions_copy[session];
    mvprintw(y,x,"%5s",sp->chan.preset);
  }
  x += 6;
  y = row_save;
  
  mvprintw(y++,x,"%5s","SNR");
  for(int session = First_session; session < Nsessions_copy; session++,y++){
    struct session const *sp = Sessions_copy[session];
    if(!isnan(sp->snr))
      mvprintw(y,x,"%5.1f",sp->snr);
  }
  x += 6;
  y = row_save;
  
  int longest = 0;
  mvprintw(y++,x,"%s","ID");
  for(int session = First_session; session < Nsessions_copy; session++,y++){
    struct session const *sp = Sessions_copy[session];
    int len = strlen(sp->id);
    if(len > longest)
      longest = len;
    mvprintw(y,x,"%s",sp->id);
  }
  x += longest;
  y = row_save;
  
  mvprintw(y++,x,"%10s","Total");
  for(int session = First_session; session < Nsessions_copy; session++,y++){
    struct session const *sp = Sessions_copy[session];
    char total_buf[100];
    mvprintw(y,x,"%10s",ftime(total_buf,sizeof(total_buf),sp->tot_active));
  }
  x += 11;
  y = row_save;
  
  mvprintw(y++,x,"%10s","Cur/idle");
  {
    long long time = gps_time_ns();
    for(int session = First_session; session < Nsessions_copy; session++,y++){
      struct session *sp = Sessions_copy[session];
      char buf[100];
      if(sp->now_active)
	mvprintw(y,x,"%10s",ftime(buf,sizeof(buf),sp->active));
      else {
	float idle_sec = (time - sp->last_active) / BILLION;
	mvprintw(y,x,"%10s",ftime(buf,sizeof(buf),idle_sec));   // Time idle since last transmission
      }
    }
  }
  x += 11;
  y = row_save;
  
  mvprintw(y++,x,"%6s","Queue");
  for(int session = First_session; session < Nsessions_copy; session++,y++){
    struct session const *sp = Sessions_copy[session];
    if(!sp->now_active || sp->muted || (Voting && Best_session != NULL && Best_session != sp))
      continue;
    
    int const d = modsub(sp->wptr,Rptr,BUFFERSIZE); // Unplayed samples on queue
    int const queue_ms = d > 0 ? 1000 * d / DAC_samprate : 0; // milliseconds
    mvprintw(y,x,"%6d",queue_ms);   // Time idle since last transmission
  }
  x += 7;
  y = row_save;
  
  if(Verbose){
    // Opus/pcm
    mvprintw(y++,x,"Type");
    for(int session = First_session; session < Nsessions_copy; session++,y++){
      struct session const *sp = Sessions_copy[session];
      mvprintw(y,x,"%4s",encoding_string(PT_table[sp->type].encoding));
    }
    x += 5;
    y = row_save;
    
    // frame size, ms
    mvprintw(y++,x,"%3s","ms");
    for(int session = First_session; session < Nsessions_copy; session++,y++){
      struct session const *sp = Sessions_copy[session];
      if(sp->samprate != 0)
	mvprintw(y,x,"%3d",(1000 * sp->frame_size/sp->samprate)); // frame size, ms
    }
    x += 4;
    y = row_save;
    
    // channels
    mvprintw(y++,x,"%2s","ch");
    for(int session = First_session; session < Nsessions_copy; session++,y++){
      struct session const *sp = Sessions_copy[session];
      mvprintw(y,x,"%2d",sp->channels);
    }
    x += 3;
    y = row_save;
    
    // BW
    mvprintw(y++,x,"%2s","bw");
    for(int session = First_session; session < Nsessions_copy; session++,y++){
      struct session const *sp = Sessions_copy[session];
      mvprintw(y,x,"%2d",sp->bandwidth);
    }
    x += 3;
    y = row_save;
    
    // RTP payload type
    mvprintw(y++,x,"%3s","pt");
    for(int session = First_session; session < Nsessions_copy; session++,y++){
      struct session const *sp = Sessions_copy[session];
      mvprintw(y,x,"%3d",sp->type);
    }
    x += 4;
    y = row_save;
    
    // Packets
    mvprintw(y++,x,"%12s","Packets");
    for(int session = First_session; session < Nsessions_copy; session++,y++){
      struct session const *sp = Sessions_copy[session];
      mvprintw(y,x,"%12lu",sp->packets);
    }
    x += 13;
    y = row_save;
    
    // Resets
    mvprintw(y++,x,"%7s","resets");
    for(int session = First_session; session < Nsessions_copy; session++,y++){
      struct session const *sp = Sessions_copy[session];
      mvprintw(y,x,"%7lu",sp->resets);
    }
    x += 8;
    y = row_save;
    
    // BW
    mvprintw(y++,x,"%6s","drops");
    for(int session = First_session; session < Nsessions_copy; session++,y++){
      struct session const *sp = Sessions_copy[session];
      mvprintw(y,x,"%'6llu",(unsigned long long)sp->rtp_state.drops);
    }
    x += 7;
    y = row_save;
    
    // Lates
    mvprintw(y++,x,"%6s","lates");
    for(int session = First_session; session < Nsessions_copy; session++,y++){
      struct session const *sp = Sessions_copy[session];
      mvprintw(y,x,"%6lu",sp->lates);
    }
    x += 7;
    y = row_save;
    
    // BW
    mvprintw(y++,x,"%6s","reseq");
    for(int session = First_session; session < Nsessions_copy; session++,y++){
      struct session const *sp = Sessions_copy[session];
      mvprintw(y,x,"%6lu",sp->reseqs);
    }
    x += 7;
    y = row_save;
    
    // Sockets
    mvprintw(y++,x,"%s","sockets");
    for(int session = First_session; session < Nsessions_copy; session++,y++){
      struct session const *sp = Sessions_copy[session];
      mvprintw(y,x,"%s -> %s",formatsock(&sp->sender),sp->dest);
    }
  }
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
