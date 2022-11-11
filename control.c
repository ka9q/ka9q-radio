// $Id: control.c,v 1.165 2022/09/16 04:06:55 karn Exp $
// Interactive program to send commands and display internal state of 'radio'
// Why are user interfaces always the biggest, ugliest and buggiest part of any program?
// Written as one big polling loop because ncurses is **not** thread safe

// Copyright 2017 Phil Karn, KA9Q
// Major revisions fall 2020

#define _GNU_SOURCE 1
#include <assert.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <stdbool.h>
#include <limits.h>
#include <string.h>
#if defined(linux)
#include <bsd/string.h>
#endif
#include <math.h>
#include <complex.h>
#undef I
#include <sys/time.h>
#include <sys/select.h>
#include <ncurses.h>
#include <ctype.h>
#include <sys/socket.h>
#include <netdb.h>
#include <locale.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <iniparser/iniparser.h>

#include "misc.h"
#include "filter.h"
#include "multicast.h"
#include "bandplan.h"
#include "status.h"
#include "radio.h"
#include "config.h"

static int const DEFAULT_IP_TOS = 48;
static int const DEFAULT_MCAST_TTL = 1;

float Refresh_rate = 0.1f;
int Mcast_ttl = DEFAULT_MCAST_TTL;
int IP_tos = DEFAULT_IP_TOS;
char Locale[256] = "en_US.UTF-8";
char const *Modefile = "modes.conf"; // make configurable!
dictionary *Mdict;

struct frontend Frontend;
char Iface[1024]; // Multicast interface to talk to front end
bool FE_address_set;
struct sockaddr_storage FE_status_address;
struct sockaddr_storage Metadata_source_address;      // Source of metadata
int Source_set;
struct sockaddr_storage Metadata_dest_address;      // Dest of metadata (typically multicast)
uint64_t Commands;
uint32_t Command_tag;
float Blocktime;
int Ctl_fd,Status_fd;
uint64_t Metadata_packets;
uint64_t Block_drops;

const char *App_path;
int Verbose;
bool Resized = false;

struct control {
  int item;
  bool lock;
  int step;
} Control;


int pprintw(WINDOW *w,int y, int x, char const *prefix, char const *fmt, ...);

WINDOW *Tuning_win,*Sig_win,*Info_win,*Filtering_win,*Demodulator_win,
  *Options_win,*Fe_win,*Modes_win,*Debug_win,
  *Data_win,*Status_win,*Output_win;

static void display_tuning(WINDOW *tuning,struct demod const *demod);
static void display_info(WINDOW *w,int row,int col,struct demod const *demod);
static void display_filtering(WINDOW *filtering,struct demod const *demod);
static void display_sig(WINDOW *sig,struct demod const *demod);
static void display_demodulator(WINDOW *demodulator,struct demod const *demod);
static void display_fe(WINDOW *fe,struct demod const *demod);
static void display_options(WINDOW *options,struct demod const *demod);
static void display_modes(WINDOW *modes,struct demod const *demod);
static void display_output(WINDOW *output,struct demod const *demod);
static int process_keyboard(struct demod *,unsigned char **bpp,int c);
static void process_mouse(struct demod *demod,unsigned char **bpp);
static int decode_radio_status(struct demod *demod,unsigned char const *buffer,int length);
static int for_us(struct demod *demod,unsigned char const *buffer,int length,uint32_t ssrc);

// Pop up a temporary window with the contents of a file in the
// library directory (usually /usr/local/share/ka9q-radio/)
// then wait for a single keyboard character to clear it
void popup(char const *filename){
  static int const maxcols = 256;
  char fname[PATH_MAX];
  if (dist_path(fname,sizeof(fname),filename) == -1)
    return;
  FILE * const fp = fopen(fname,"r");
  if(fp == NULL)
    return;
  // Determine size of box
  int rows=0, cols=0;
  char line[maxcols];
  while(fgets(line,sizeof(line),fp) != NULL){
    chomp(line);
    rows++;
    if(strlen(line) > cols)
      cols = strlen(line); // Longest line
  }
  rewind(fp);
  
  // Allow room for box
  WINDOW * const pop = newwin(rows+2,cols+2,0,0);
  box(pop,0,0);
  int row = 1; // Start inside box
  while(fgets(line,sizeof(line),fp) != NULL){
    chomp(line);
    mvwaddstr(pop,row++,1,line);
  }
  fclose(fp);
  wnoutrefresh(pop);
  doupdate();
  wtimeout(pop,-1); // blocking read - wait indefinitely
  (void)wgetch(pop); // Read and discard one character
  wtimeout(pop,0);
  werase(pop);
  wrefresh(pop);
  delwin(pop);
}


// Pop up a dialog box, issue a prompt and get a response
void getentry(char const *prompt,char *response,int len){
  WINDOW * const pwin = newwin(5,90,0,0);
  box(pwin,0,0);
  mvwaddstr(pwin,1,1,prompt);
  wrefresh(pwin);
  echo();
  timeout(-1);
  // Manpage for wgetnstr doesn't say whether a terminating
  // null is stashed. Hard to believe it isn't, but this is to be sure
  memset(response,0,len);
  wgetnstr(pwin,response,len);
  chomp(response);
  timeout(0);
  noecho();
  werase(pwin);
  wrefresh(pwin);
  delwin(pwin);
}

static FILE *Tty;
static SCREEN *Term;

void display_cleanup(void){
  echo();
  nocbreak();
  if(!isendwin()){
    endwin();
    refresh();
  }
  if(Term)
    delscreen(Term);
  Term = NULL;
  if(Tty)
    fclose(Tty);
  Tty = NULL;
}

static bool Frequency_lock;

// Adjust the selected item up or down one step
void adjust_item(struct demod *demod,unsigned char **bpp,int direction){
  double tunestep = pow(10., (double)Control.step);

  if(!direction)
    tunestep = - tunestep;

  switch(Control.item){
  case 0: // Carrier frequency
    if(!Frequency_lock){ // Ignore if locked
      demod->tune.freq += tunestep;
      encode_double(bpp,RADIO_FREQUENCY,demod->tune.freq);
    }
    break;
  case 1: // First LO
    if(Control.lock) // Tuner is locked, don't change it
      break;
    // Send directly to first LO
    set_first_LO(demod,Frontend.sdr.frequency+tunestep);
    break;
  case 2: // IF (not implemented)
    break;
  case 3: // Filter low edge (hertz rather than millihertz)
    {
      float const x = min(demod->filter.max_IF,demod->filter.min_IF + (float)tunestep * 1000);
      demod->filter.min_IF = x;
      encode_float(bpp,LOW_EDGE,x);
    }
    break;
  case 4: // Filter high edge
    {
      float const x = max(demod->filter.min_IF,demod->filter.max_IF + (float)tunestep * 1000);
      demod->filter.max_IF = x;
      encode_float(bpp,HIGH_EDGE,x);
    }
    break;
  case 5: // Post-detection audio frequency shift
    demod->tune.shift += tunestep;
    encode_double(bpp,SHIFT_FREQUENCY,demod->tune.shift);
    break;

  }
}
// Hooks for knob.c (experimental)
// It seems better to just use the Griffin application to turn knob events into keystrokes or mouse events
void adjust_up(struct demod *demod,unsigned char **bpp){
  adjust_item(demod,bpp,1);
}
void adjust_down(struct demod *demod,unsigned char **bpp){
  adjust_item(demod,bpp,0);
}
void toggle_lock(void){
  switch(Control.item){
  case 0:
    Frequency_lock = !Frequency_lock; // Toggle frequency tuning lock
    break;
  case 1:
    Control.lock = !Control.lock;
    break;
  }
}
// List of status windows, in order they'll be created, with sizes
struct windef {
  WINDOW **w;
  int rows;
  int cols;
} Windefs[] = {
  {&Tuning_win, 15, 30},
  {&Options_win, 15, 12},  
  //  {&Modes_win,Npresets+2,9}, // Npresets is not a static initializer
  {&Modes_win,15,9},
  {&Sig_win,15,25},
  {&Demodulator_win,15,26},
  {&Filtering_win,15,22},
  {&Fe_win,15,45},
  {&Output_win,11,45},
  {&Debug_win,8,109},
};
#define NWINS (sizeof(Windefs) / sizeof(Windefs[0]))

void setup_windows(void){
  // First row
  int row = 0;
  int col = 0;
  int maxrows = 0;

  endwin();
  refresh();
  clear();

  struct winsize w;
  ioctl(fileno(Tty),TIOCGWINSZ,&w);
  COLS = w.ws_col;
  LINES = w.ws_row;
  
  // Delete all previous windows
  for(int i=0; i < NWINS; i++){
    if(*Windefs[i].w)
      delwin(*Windefs[i].w);
    *Windefs[i].w = NULL;
  }
  // Create as many as will fit
  for(int i=0; i < NWINS; i++){  
    if(COLS < col + Windefs[i].cols){
      // No more room on this line, go to next
      col = 0;
      row += maxrows;
      maxrows = 0;
    }      
    if(LINES < row + Windefs[i].rows){
      // No more room for anything
      return;
    }
    // Room on this line
    * Windefs[i].w = newwin(Windefs[i].rows,Windefs[i].cols,row,col);
    col += Windefs[i].cols;
    maxrows = max(maxrows,Windefs[i].rows);
  }
  if(Debug_win != NULL){
    // A message from our sponsor...
    wprintw(Debug_win,"KA9Q SDR Receiver controller\nCopyright 2022 Phil Karn, KA9Q\n");
  }
}

void winch_handler(int num){
  Resized = true;
}

static int dcompare(void const *a,void const *b){
  struct demod const *da = a;
  struct demod const *db = b;  
  if(da->output.rtp.ssrc < db->output.rtp.ssrc)
    return -1;
  if(da->output.rtp.ssrc > db->output.rtp.ssrc)
    return +1;
  return 0;
}



uint32_t Ssrc = 0;

// Thread to display receiver state, updated at 10Hz by default
// Uses the ancient ncurses text windowing library
// Also services keyboard, mouse and tuning knob, if present
// I had been running this at normal priority, but it can start new demodulators
// so it must also run at preferred priority
int main(int argc,char *argv[]){
  App_path = argv[0];
  {
    int c;
    while((c = getopt(argc,argv,"vs:")) != -1){
      switch(c){
      case 'v':
	Verbose++;
	break;
      case 's':
	Ssrc = strtol(optarg,NULL,0); // Send to specific SSRC
	break;
      default:
	fprintf(stdout,"Unknown option %c\n",c);
	break;
      }
    }
  }
  {
    // The display thread assumes en_US.UTF-8, or anything with a thousands grouping character
    // Otherwise the cursor movements will be wrong
    char const * const cp = getenv("LANG");
    if(cp != NULL){
      strlcpy(Locale,cp,sizeof(Locale));
    }
  }
  setlocale(LC_ALL,Locale); // Set either the hardwired default or the value of $LANG if it exists
  // Dummy filter

  resolve_mcast(argv[optind],&Metadata_dest_address,DEFAULT_STAT_PORT,Iface,sizeof(Iface));
  Status_fd = listen_mcast(&Metadata_dest_address,Iface);
  if(Status_fd == -1){
    fprintf(stderr,"Can't listen to mcast status %s\n",argv[optind]);
    exit(1);
  }
  Ctl_fd = connect_mcast(&Metadata_dest_address,Iface,Mcast_ttl,IP_tos);
  if(Ctl_fd < 0){
    fprintf(stderr,"connect to mcast control failed\n");
    exit(1);
  }
  if(Ssrc == 0){
    // no ssrc specified; send wild-card poll and collect responses
    unsigned ssrc_count = 0;
    struct demod *demods = NULL;
    unsigned demods_size = 0;
    // The deadline starts at 1 sec in the future
    // It is reset as long as we keep seeing new SSRCs
    long long deadline = gps_time_ns() + BILLION;

    send_poll(Ctl_fd,0);

    while(1){
      fd_set fdset;
      FD_ZERO(&fdset);
      FD_SET(Status_fd,&fdset);
      int n = Status_fd + 1;
      
      long long timeout = deadline - gps_time_ns();
      // Immediate poll if timeout is negative
      if(timeout < 0)
	timeout = 0;

      {
	struct timespec ts;
	ns2ts(&ts,timeout);
	n = pselect(n,&fdset,NULL,NULL,&ts,NULL);
      }
      if(n <= 0)
	break; // Only on a timeout
      if(!FD_ISSET(Status_fd,&fdset))
	continue;

      // Message from the radio program
      unsigned char buffer[8192];
      socklen_t ssize = sizeof(Metadata_source_address);
      int const length = recvfrom(Status_fd,buffer,sizeof(buffer),0,(struct sockaddr *)&Metadata_source_address,&ssize);
      
      // Ignore our own command packets
      if(length < 2 || buffer[0] != 0)
	continue;
      int const ssrc = get_ssrc(buffer+1,length-1);
      // See if it's a dupe (e.g., response to another instance of control)
      for(int i=0; i < ssrc_count; i++){
	if(demods[i].output.rtp.ssrc == ssrc)
	  goto ignore;
      }
      if(ssrc_count >= demods_size){
	// Enlarge demods array
	demods_size += 1000;
	demods = realloc(demods,demods_size * sizeof(*demods));
      }
      decode_radio_status(&demods[ssrc_count],buffer+1,length-1);
      ssrc_count++;
      deadline = gps_time_ns() + BILLION; // extend deadline as long as we're progressing
    ignore:;
    }

    qsort(demods,ssrc_count,sizeof(*demods),dcompare);
    fprintf(stdout,"SSRC      Frequency (Hz)\n");
    for(int i=0; i < ssrc_count; i++){
      fprintf(stdout,"%-9u %'.3lf\n",demods[i].output.rtp.ssrc,
	      demods[i].tune.freq);
    }
    fprintf(stdout,"Total SSRCs: %u\n",ssrc_count);
    free(demods);
    demods = NULL;
    exit(0);
  }

  char modefile_path[PATH_MAX];
  if (dist_path(modefile_path,sizeof(modefile_path),Modefile) == -1) {
    fprintf(stderr,"Could not find mode file %s\n", Modefile);
    exit(1);
  }
  Mdict = iniparser_load(modefile_path);
  if(Mdict == NULL){
    fprintf(stdout,"Can't load mode file %s\n",modefile_path);
    exit(1);
  }


  atexit(display_cleanup);

  struct sigaction act;
  memset(&act,0,sizeof(act));
  act.sa_handler = winch_handler;
  sigaction(SIGWINCH,&act,NULL);

  // Set up display subwindows
  Tty = fopen("/dev/tty","r+");
  Term = newterm(NULL,Tty,Tty);
  set_term(Term);

  //  meta(stdscr,TRUE);
  keypad(stdscr,TRUE);
  timeout(0); // Don't block in getch()
  cbreak();
  noecho();

  mmask_t const mask = ALL_MOUSE_EVENTS;
  mousemask(mask,NULL);

  setup_windows();

  struct demod demod_actual;
  struct demod *const demod = &demod_actual;
  memset(demod,0,sizeof(*demod));
  init_demod(demod);

  Frontend.sdr.frequency = 
    Frontend.sdr.min_IF = Frontend.sdr.max_IF = NAN;
  Frontend.input.data_fd = Frontend.input.status_fd = -1;

  /* Main loop:
     Read radio/front end status from network with 100 ms timeout to serve as polling rate
     Update local status
     Repaint display windows
     Poll keyboard and process user commands
  */

  // In case there's another control program on the same channel,
  // randomize polls over 50 ms and restart our poll timer if an answer
  // is seen in response to another poll

  long long const random_interval = 50000000; // 50 ms
  // Pick soon but still random times for the first polls
  long long next_radio_poll = random_time(0,random_interval);
  long long next_fe_poll = random_time(0,random_interval);
  
  for(;;){
    long long const radio_poll_interval  = Refresh_rate * BILLION;
    long long const fe_poll_interval = 975000000;    // 975 - 1025 ms

    if(gps_time_ns() > next_radio_poll){
      // Time to poll radio
      send_poll(Ctl_fd,Ssrc);
      next_radio_poll = random_time(radio_poll_interval,random_interval);
    }
    if(gps_time_ns() > next_fe_poll){
      // Time to poll front end
      if(Frontend.input.ctl_fd > 2)
	send_poll(Frontend.input.ctl_fd,0);
      next_fe_poll = random_time(fe_poll_interval,random_interval);
    }
    fd_set fdset;
    FD_ZERO(&fdset);
    if(Frontend.input.status_fd != -1)
      FD_SET(Frontend.input.status_fd,&fdset);
    FD_SET(Status_fd,&fdset);
    int const n = max(Frontend.input.status_fd,Status_fd) + 1;

    // Receive timeout at whichever event occurs first
    long long timeout;
    if(next_radio_poll > next_fe_poll)
      timeout = next_fe_poll - gps_time_ns();
    else
      timeout = next_radio_poll - gps_time_ns();

    // Immediate poll if timeout is negative
    if(timeout < 0)
      timeout = 0;
    {
      struct timespec ts;
      ns2ts(&ts,timeout);
      pselect(n,&fdset,NULL,NULL,&ts,NULL); // Don't really need to check the return
    }
    if(Resized){
      Resized = false;
      setup_windows();
    }
    if(Status_fd != -1 && FD_ISSET(Status_fd,&fdset)){
      // Message from the radio program (or some transcoders)
      unsigned char buffer[8192];
      socklen_t ssize = sizeof(Metadata_source_address);
      int length = recvfrom(Status_fd,buffer,sizeof(buffer),0,(struct sockaddr *)&Metadata_source_address,&ssize);

      // Ignore our own command packets and responses to other SSIDs
      if(length >= 2 && buffer[0] == 0 && for_us(demod,buffer+1,length-1,Ssrc) >= 0 ){
	decode_radio_status(demod,buffer+1,length-1);
	next_radio_poll = random_time(radio_poll_interval,random_interval);

	if(Frontend.sdr.samprate != 0)
	  Blocktime = 1000.0f * Frontend.L / Frontend.sdr.samprate;

	// Listen directly to the front end once we know its multicast group
	if(!FE_address_set){
	  FE_address_set = true;
	  if(Frontend.input.status_fd >= 0){
	    close(Frontend.input.status_fd);
	    Frontend.input.status_fd = -1;
	  }
	  if(Frontend.input.ctl_fd >= 0){
	    close(Frontend.input.ctl_fd);
	    Frontend.input.ctl_fd = -1;
	  }
	  Frontend.input.status_fd = listen_mcast(&Frontend.input.metadata_dest_address,Iface);
	  Frontend.input.ctl_fd = connect_mcast(&Frontend.input.metadata_dest_address,Iface,Mcast_ttl,IP_tos);
	}
      }
    }
    if(Frontend.input.status_fd != -1 && FD_ISSET(Frontend.input.status_fd,&fdset)){
      // Message from the front end
      unsigned char buffer[8192];
      struct sockaddr_storage sender;
      socklen_t ssize = sizeof(sender);
      int const length = recvfrom(Frontend.input.status_fd,buffer,sizeof(buffer),0,(struct sockaddr *)&sender,&ssize);
      socklen_t msize = sizeof(Frontend.input.metadata_source_address);
      msize = min(ssize,msize);

#if 0 // compare fails on MacOS, more trouble than it's worth
      // Filter possible stray packets from unexpected sources
      if(FE_address_set // should aways be true here 
	 && memcmp(&sender,&Frontend.input.metadata_source_address,msize) == 0
	 && length >= 2 && buffer[0] == 0){
	// Parse entries
	decode_fe_status(&Frontend,buffer+1,length-1);
	next_fe_poll = random_time(fe_poll_interval,random_interval);
      }
#else
      if(length >= 2 && buffer[0] == 0){
	decode_fe_status(&Frontend,buffer+1,length-1);
	next_fe_poll = random_time(fe_poll_interval,random_interval);
      }
#endif
    }
    // socket read timeout every 100 ms; update display windows & poll keyboard and mouse
    display_tuning(Tuning_win,demod);
    display_filtering(Filtering_win,demod);
    display_sig(Sig_win,demod);
    display_demodulator(Demodulator_win,demod);
    display_fe(Fe_win,demod);
    display_options(Options_win,demod);
    display_modes(Modes_win,demod);
    display_output(Output_win,demod);
    
    if(Debug_win != NULL){
      touchwin(Debug_win); // since we're not redrawing it every cycle
      wnoutrefresh(Debug_win);
    }    
    doupdate();      // Update the screen right before we pause
    
    // Set up command buffer in case we want to change something
    unsigned char cmdbuffer[1024];
    unsigned char *bp = cmdbuffer;
    *bp++ = 1; // Command

    int const c = getch(); // read keyboard with timeout; controls refresh rate
    if(c == KEY_MOUSE){
      process_mouse(demod,&bp);
    } else if(c != ERR) {
      if(process_keyboard(demod,&bp,c) == -1)
	goto quit;
    }

    // OK, any commands to send?
    if(bp > cmdbuffer+1){
      // Yes
      if(Ssrc != 0)
	encode_int(&bp,OUTPUT_SSRC,Ssrc); // Specific SSRC
      encode_int(&bp,COMMAND_TAG,random()); // Append a command tag
      encode_eol(&bp);
      int const command_len = bp - cmdbuffer;
      if(send(Ctl_fd, cmdbuffer, command_len, 0) != command_len)
	perror("command send");
    }
  }
 quit:;
  endwin();
  set_term(NULL);
  if(Term != NULL)
    delscreen(Term);
  //  if(Tty != NULL)
  //    fclose(Tty);
  
  exit(0);
}


int process_keyboard(struct demod *demod,unsigned char **bpp,int c){
  // Look for keyboard and mouse events

  switch(c){
  case ERR:
    break;
  case KEY_RESIZE:
    break; // Ignore
  case 0x3: // ^C
  case 'q':   // Exit entire radio program. Should this be removed? ^C also works.
    return -1;
  case 'h':
  case '?':
    popup("help.txt");
    break;
  case 'l': // Toggle RF or first LO lock; affects how adjustments to LO and IF behave
    toggle_lock();
    break;
  case KEY_NPAGE: // Page Down/tab key
  case '\t':      // go to next tuning item
    Control.item = (Control.item + 1) % 6;
    break;
  case KEY_BTAB:  // Page Up/Backtab, i.e., shifted tab:
  case KEY_PPAGE: // go to previous tuning item
    Control.item = (6 + Control.item - 1) % 6;
    break;
  case KEY_HOME: // Go back to item 0
    Control.item = 0;
    Control.step = 0;
    break;
  case KEY_BACKSPACE: // Cursor left: increase tuning step 10x
  case KEY_LEFT:
    if(Control.step >= 9){
      beep();
      break;
    }
    Control.step++;
    break;
  case KEY_RIGHT:     // Cursor right: decrease tuning step /10
    if(Control.step <= -3){
      beep();
      break;
    }
    Control.step--;
    break;
  case KEY_UP:        // Increase whatever digit we're tuning
    adjust_up(demod,bpp);
    break;
  case KEY_DOWN:      // Decrease whatever we're tuning
    adjust_down(demod,bpp);
    break;
  case '\f':  // Screen repaint (formfeed, aka control-L)
    clearok(curscr,TRUE);
    break;
  case 's': // Squelch threshold for current mode
    {
      char str[1024],*ptr;
      getentry("Squelch SNR: ",str,sizeof(str));
      float const x = strtof(str,&ptr);
      if(ptr != str){
	encode_float(bpp,SQUELCH_OPEN,x);
	encode_float(bpp,SQUELCH_CLOSE,x - 1); // Make this a separate command
      }
    }
    break;
  case 'T': // Hang time, s (always taken as positive)
    {
      char str[1024],*ptr;
      getentry("Hang time, s: ",str,sizeof(str));
      float const x = fabsf(strtof(str,&ptr));
      if(ptr != str)
	encode_float(bpp,AGC_HANGTIME,x);
    }
    break;
  case 'P': // PLL loop bandwidth
    {
      char str[1024],*ptr;
      getentry("PLL loop bandwidth, Hz: ",str,sizeof(str));
      float const x = fabsf(strtof(str,&ptr));
      if(ptr != str)
	encode_float(bpp,PLL_BW,x);
    }
    break;
  case 'L': // AGC threshold, dB relative to headroom
    {
      char str[1024],*ptr;
      getentry("AGC threshold, dB: ",str,sizeof(str));
      float const x = strtof(str,&ptr);
      if(ptr != str)
	encode_float(bpp,AGC_THRESHOLD,x);
    }
    break;
  case 'R': // Recovery rate, dB/s (always taken as positive)
    {
      char str[1024],*ptr;
      getentry("Recovery rate, dB/s: ",str,sizeof(str));
      float const x = fabsf(strtof(str,&ptr));
      if(ptr != str)
	encode_float(bpp,AGC_RECOVERY_RATE,x);
    }
    break;
  case 'H': // Headroom, dB (taken as negative)
    {
      char str[1024],*ptr;
      getentry("Headroom, dB: ",str,sizeof(str));
      float const x = -fabsf(strtof(str,&ptr));
      if(ptr != str)
	encode_float(bpp,HEADROOM,x);
    }
    break;
  case 'g': // Manually set linear demod gain, dB (positive or negative)
    {
      char str[1024],*ptr;
      getentry("Gain, dB: ",str,sizeof(str));
      float const x = strtof(str,&ptr);
      if(ptr != str)
	encode_float(bpp,GAIN,x);
    }
    break;
  case 'r':
    {
      char str[1024],*ptr;
      getentry("Refresh rate (s): ",str,sizeof(str));
      float const x = strtof(str,&ptr);
      Refresh_rate = x;
    }
    break;
  case 'm': // Manually set modulation mode
    {
      char str[1024];
      snprintf(str,sizeof(str),"Mode [ ");
      int const nsec = iniparser_getnsec(Mdict);

      for(int i=0;i < nsec;i++){
	strlcat(str,iniparser_getsecname(Mdict,i),sizeof(str));
	strlcat(str," ",sizeof(str));
      }
      strlcat(str,"]: ",sizeof(str));
      getentry(str,str,sizeof(str));
      if(strlen(str) > 0)
	encode_string(bpp,PRESET,str,strlen(str));
    }
    break;
  case 'f':   // Tune to new radio frequency
    {
      char str[160];
      getentry("Carrier frequency: ",str,sizeof(str));
      if(strlen(str) > 0){
	double const f = fabs(parse_frequency(str)); // Handles funky forms like 147m435
	
	// If frequency would be out of range, guess kHz or MHz
	if(f >= 0.1 && f < 100)
	  demod->tune.freq = f*1e6; // 0.1 - 99.999 Only MHz can be valid
	else if(f < 500)         // 100-499.999 could be kHz or MHz, assume MHz
	  demod->tune.freq = f*1e6;
	else if(f < 2000)        // 500-1999.999 could be kHz or MHz, assume kHz
	  demod->tune.freq = f*1e3;
	else if(f < 100000)      // 2000-99999.999 can only be kHz
	  demod->tune.freq = f*1e3;
	else                     // accept directly
	  demod->tune.freq = f;
	encode_double(bpp,RADIO_FREQUENCY,demod->tune.freq);
      }
    }
    break;
  case 'k': // Kaiser window parameter
    {
      char str[160],*ptr;
      getentry("Kaiser window beta: ",str,sizeof(str));
      double const b = strtod(str,&ptr);
      if(ptr == str)
	break; // nothing entered
      if(b < 0 || b >= 100){
	beep();
	break; // beyond limits
      }
      encode_float(bpp,KAISER_BETA,b);
    }
    break;
  case 'o': // Set/clear option flags, most apply only to linear detector
    {
      char str[160];
      getentry("[isb pll square stereo mono agc], '!' prefix disables: ",str,sizeof(str));
      if(strcasecmp(str,"mono") == 0){
	encode_int(bpp,OUTPUT_CHANNELS,1);
      } else if(strcasecmp(str,"!mono") == 0){
	encode_int(bpp,OUTPUT_CHANNELS,2);
      } else if(strcasecmp(str,"stereo") == 0){
	encode_int(bpp,OUTPUT_CHANNELS,2);
      } else if(strcasecmp(str,"isb") == 0){
	encode_byte(bpp,INDEPENDENT_SIDEBAND,1);
      } else if(strcasecmp(str,"!isb") == 0){
	encode_byte(bpp,INDEPENDENT_SIDEBAND,0);
      } else if(strcasecmp(str,"pll") == 0){
	encode_byte(bpp,PLL_ENABLE,1);
      } else if(strcasecmp(str,"!pll") == 0){
	encode_byte(bpp,PLL_ENABLE,0);
	encode_byte(bpp,PLL_SQUARE,0);	
      } else if(strcasecmp(str,"square") == 0){
	encode_byte(bpp,PLL_ENABLE,1);
	encode_byte(bpp,PLL_SQUARE,1);	
      } else if(strcasecmp(str,"!square") == 0){	  
	encode_byte(bpp,PLL_SQUARE,0);	
      } else if(strcasecmp(str,"agc") == 0){
	encode_byte(bpp,AGC_ENABLE,1);
      } else if(strcasecmp(str,"!agc") == 0){
	encode_byte(bpp,AGC_ENABLE,0);
      }
    }
    break;
  default:
    beep();
    break;
  } // switch
  return 0;
}

void process_mouse(struct demod *demod,unsigned char **bpp){
  // Process mouse events
  // Need to handle the wheel as equivalent to up/down arrows
  MEVENT mouse_event;

  getmouse(&mouse_event);
  int mx,my;
  mx = mouse_event.x;
  my = mouse_event.y;
  mouse_event.y = mouse_event.x = mouse_event.z = 0;
  if(mx != 0 && my != 0){
#if 0
    wprintw(debug," (%d %d)",mx,my);
#endif
    if(Tuning_win && wmouse_trafo(Tuning_win,&my,&mx,false)){
      // Tuning window
      Control.item = my-1;
      Control.step = 24-mx;
      if(Control.step < 0)
	Control.step++;
      if(Control.step > 3)
	Control.step--;
      if(Control.step > 6)
	Control.step--;
      if(Control.step > 9)	
	Control.step--;
      // Clamp to range
      if(Control.step < -3)
	Control.step = -3;
      if(Control.step > 9)
	Control.step = 9;
	  
    } else if(Modes_win && wmouse_trafo(Modes_win,&my,&mx,false)){
      // In the modes window?
      my--;
      if(my >= 0 && my < iniparser_getnsec(Mdict)){
	char const *n = iniparser_getsecname(Mdict,my);
	encode_string(bpp,PRESET,n,strlen(n));
      }
	  
    } else if(Options_win && wmouse_trafo(Options_win,&my,&mx,false)){
      // In the options window
      if(demod->demod_type == WFM_DEMOD){
	switch(my){
	case 1:
	  encode_int(bpp,OUTPUT_CHANNELS,1);
	  break;
	case 2:
	  encode_int(bpp,OUTPUT_CHANNELS,2);
	  break;
	}
      } else if(demod->demod_type == FM_DEMOD){
      } else if(demod->demod_type == LINEAR_DEMOD){
	switch(my){
	case 1:
	  encode_int(bpp,ENVELOPE,1);
	  encode_int(bpp,OUTPUT_CHANNELS,1);
	  break;
	case 2:
	  encode_int(bpp,ENVELOPE,1);
	  encode_int(bpp,OUTPUT_CHANNELS,2);
	  break;
 	case 3:
	  encode_int(bpp,ENVELOPE,0);
	  encode_int(bpp,OUTPUT_CHANNELS,1);
	  break;
	case 4:
	  encode_int(bpp,ENVELOPE,0);
	  encode_int(bpp,OUTPUT_CHANNELS,2);
	  break;
	case 5:
	  encode_int(bpp,PLL_ENABLE,0);
	  break;
	case 6:
	  encode_int(bpp,PLL_ENABLE,1);
	  encode_int(bpp,PLL_SQUARE,0);
	  break;
	case 7:	  
	  encode_int(bpp,PLL_ENABLE,1);
	  encode_int(bpp,PLL_SQUARE,1);
	  break;
	case 8:
	  encode_int(bpp,AGC_ENABLE,0);
	  break;
	case 9:
	  encode_int(bpp,AGC_ENABLE,1);
	  break;
	}
      }
    }
  } // end of mouse processing
}

// Initialize a new, unused demod instance where fields start non-zero
int init_demod(struct demod *demod){
  memset(demod,0,sizeof(*demod));
  demod->tune.second_LO = NAN;
  demod->tune.freq = demod->tune.shift = NAN;
  demod->filter.min_IF = demod->filter.max_IF = demod->filter.kaiser_beta = NAN;
  demod->output.headroom = demod->linear.hangtime = demod->linear.recovery_rate = NAN;
  demod->sig.bb_power = demod->sig.snr = demod->sig.foffset = NAN;
  demod->fm.pdeviation = demod->linear.cphase = demod->linear.lock_timer = NAN;
  demod->output.gain = NAN;
  demod->tp1 = demod->tp2 = NAN;

  demod->output.data_fd = demod->output.rtcp_fd = -1;
  return 0;
}

// Is response for us (1), or for somebody else (-1)?
static int for_us(struct demod *demod,unsigned char const *buffer,int length,uint32_t ssrc){
  unsigned char const *cp = buffer;
  
  while(cp - buffer < length){
    enum status_type const type = *cp++; // increment cp to length field
    
    if(type == EOL)
      break; // end of list, no length
    
    unsigned int const optlen = *cp++;
    if(cp - buffer + optlen >= length)
      break; // invalid length; we can't continue to scan
    
    switch(type){
    case EOL: // Shouldn't get here
      goto done;
    case OUTPUT_SSRC: // If we've specified a SSRC, it must match it
      if(ssrc != 0){
	if(decode_int(cp,optlen) == ssrc)
	  return 1; // For us
	return -1; // For someone else
      }
      break;
    default:
      break; // Ignore on this pass
    }
    cp += optlen;
  }
 done:;
  return 0; // not specified
}

// Decode incoming status message from the radio program, convert and fill in fields in local demod structure
// Leave all other fields unchanged, as they may have local uses (e.g., file descriptors)
int decode_radio_status(struct demod *demod,unsigned char const *buffer,int length){
  unsigned char const *cp = buffer;
  while(cp - buffer < length){
    enum status_type type = *cp++; // increment cp to length field

    if(type == EOL)
      break; // end of list
    
    unsigned int optlen = *cp++;
    if(cp - buffer + optlen >= length)
      break; // invalid length; we can't continue to scan
    switch(type){
    case EOL:
      break;
    case CMD_CNT:
      Commands = decode_int(cp,optlen);
      break;
    case DESCRIPTION:
      decode_string(cp,optlen,Frontend.sdr.description,sizeof(Frontend.sdr.description));
      break;
    case GPS_TIME:
      Frontend.sdr.timestamp = decode_int(cp,optlen);
      break;
    case INPUT_DATA_SOURCE_SOCKET:
      decode_socket(&Frontend.input.data_source_address,cp,optlen);
      break;
    case INPUT_DATA_DEST_SOCKET:
      decode_socket(&Frontend.input.data_dest_address,cp,optlen);
      break;
    case INPUT_METADATA_SOURCE_SOCKET:
      decode_socket(&Frontend.input.metadata_source_address,cp,optlen);
      break;
    case INPUT_METADATA_DEST_SOCKET:
      decode_socket(&Frontend.input.metadata_dest_address,cp,optlen);
      break;
    case INPUT_SSRC:
      Frontend.input.rtp.ssrc = decode_int(cp,optlen);
      break;
    case INPUT_SAMPRATE:
      Frontend.sdr.samprate = decode_int(cp,optlen);
      break;
    case INPUT_METADATA_PACKETS:
      Frontend.input.metadata_packets = decode_int(cp,optlen);
      break;
    case INPUT_DATA_PACKETS:
      Frontend.input.rtp.packets = decode_int(cp,optlen);
      break;
    case INPUT_SAMPLES:
      Frontend.input.samples = decode_int(cp,optlen);
      break;
    case INPUT_DROPS:
      Frontend.input.rtp.drops = decode_int(cp,optlen);
      break;
    case INPUT_DUPES:
      Frontend.input.rtp.dupes = decode_int(cp,optlen);
      break;
    case OUTPUT_DATA_SOURCE_SOCKET:
      decode_socket(&demod->output.data_source_address,cp,optlen);
      break;
    case OUTPUT_DATA_DEST_SOCKET:
      decode_socket(&demod->output.data_dest_address,cp,optlen);
      break;
    case OUTPUT_SSRC:
      demod->output.rtp.ssrc = decode_int(cp,optlen);
      break;
    case OUTPUT_TTL:
      Mcast_ttl = decode_int(cp,optlen);
      break;
    case OUTPUT_SAMPRATE:
      demod->output.samprate = decode_int(cp,optlen);
      break;
    case OUTPUT_DATA_PACKETS:
      demod->output.rtp.packets = decode_int(cp,optlen);
      break;
    case OUTPUT_METADATA_PACKETS:
      Metadata_packets = decode_int(cp,optlen);      
      break;
    case FILTER_BLOCKSIZE:
      Frontend.L = decode_int(cp,optlen);
      break;
    case FILTER_FIR_LENGTH:
      Frontend.M = decode_int(cp,optlen);
      break;
    case LOW_EDGE:
      demod->filter.min_IF = decode_float(cp,optlen);
      break;
    case HIGH_EDGE:
      demod->filter.max_IF = decode_float(cp,optlen);
      break;
    case KAISER_BETA:
      demod->filter.kaiser_beta = decode_float(cp,optlen);
      break;
    case FILTER_DROPS:
      Block_drops = decode_int(cp,optlen);
      break;
    case IF_POWER:
      // Can also be filled in by front end, though some don't send it
      Frontend.sdr.output_level = dB2power(decode_float(cp,optlen));
      break;
    case BASEBAND_POWER:
      demod->sig.bb_power = dB2power(decode_float(cp,optlen)); // dB -> power
      break;
    case NOISE_DENSITY:
      Frontend.n0 = dB2power(decode_float(cp,optlen));
      break;
    case DEMOD_SNR:
      demod->sig.snr = dB2power(decode_float(cp,optlen));
      break;
    case FREQ_OFFSET:
      demod->sig.foffset = decode_float(cp,optlen);
      break;
    case PEAK_DEVIATION:
      demod->fm.pdeviation = decode_float(cp,optlen);
      break;
    case PLL_LOCK:
      demod->linear.pll_lock = decode_int(cp,optlen);
      break;
    case PLL_BW:
      demod->linear.loop_bw = decode_float(cp,optlen);
      break;
    case PLL_SQUARE:
      demod->linear.square = decode_int(cp,optlen);
      break;
    case PLL_PHASE:
      demod->linear.cphase = decode_float(cp,optlen);
      break;
    case ENVELOPE:
      demod->linear.env = decode_int(cp,optlen);
      break;
    case OUTPUT_LEVEL:
      demod->output.level = dB2power(decode_float(cp,optlen));
      break;
    case OUTPUT_SAMPLES:
      demod->output.samples = decode_int(cp,optlen);
      break;
    case COMMAND_TAG:
      Command_tag = decode_int(cp,optlen);
      break;
    case RADIO_FREQUENCY:
      demod->tune.freq = decode_double(cp,optlen);
      break;
    case SECOND_LO_FREQUENCY:
      demod->tune.second_LO = decode_double(cp,optlen);
      break;
    case SHIFT_FREQUENCY:
      demod->tune.shift = decode_double(cp,optlen);
      break;
    case FIRST_LO_FREQUENCY:
      Frontend.sdr.frequency = decode_double(cp,optlen);
      break;
    case DOPPLER_FREQUENCY:
      demod->tune.doppler = decode_double(cp,optlen);
      break;
    case DOPPLER_FREQUENCY_RATE:
      demod->tune.doppler_rate = decode_double(cp,optlen);
      break;
    case DEMOD_TYPE:
      demod->demod_type = decode_int(cp,optlen);
      break;
    case OUTPUT_CHANNELS:
      demod->output.channels = decode_int(cp,optlen);
      break;
    case INDEPENDENT_SIDEBAND:
      demod->filter.isb = decode_int(cp,optlen);
      break;
    case PLL_ENABLE:
      demod->linear.pll = decode_int(cp,optlen);
      break;
    case GAIN:              // dB to voltage
      demod->output.gain = dB2voltage(decode_float(cp,optlen));
      break;
    case AGC_ENABLE:
      demod->linear.agc = decode_int(cp,optlen);
      break;
    case HEADROOM:          // db to voltage
      demod->output.headroom = dB2voltage(decode_float(cp,optlen));
      break;
    case AGC_HANGTIME:      // s to samples
      demod->linear.hangtime = decode_float(cp,optlen);
      break;
    case AGC_RECOVERY_RATE: // dB/s to dB/sample to voltage/sample
      demod->linear.recovery_rate = dB2voltage(decode_float(cp,optlen));
      break;
    case AGC_THRESHOLD:   // dB to voltage
      demod->linear.threshold = dB2voltage(decode_float(cp,optlen));
      break;
    case TP1: // Test point
      demod->tp1 = decode_float(cp,optlen);
      break;
    case TP2:
      demod->tp2 = decode_float(cp,optlen);
      break;
    case SQUELCH_OPEN:
      demod->squelch_open = dB2power(decode_float(cp,optlen));
      break;
    case SQUELCH_CLOSE:
      demod->squelch_close = dB2power(decode_float(cp,optlen));
      break;
    case DEEMPH_GAIN:
      demod->deemph.gain = decode_float(cp,optlen);
      break;
    case DEEMPH_TC:
      demod->deemph.rate = 1e6*decode_float(cp,optlen);
      break;
    case PL_TONE:
      demod->fm.tone_freq = decode_float(cp,optlen);
      break;
    case PL_DEVIATION:
      demod->fm.tone_deviation = decode_float(cp,optlen);
      break;
    default: // ignore others
      break;
    }
    cp += optlen;
  }
  return 0;
}

void display_tuning(WINDOW *w,struct demod const *demod){
  // Tuning control window - these can be adjusted by the user
  // using the keyboard or tuning knob, so be careful with formatting
  if(w == NULL)
    return;

  wmove(w,0,0);
  wclrtobot(w);
  int row = 1;
  int col = 1;
  if(Frequency_lock)
    wattron(w,A_UNDERLINE); // Underscore means the frequency is locked
  pprintw(w,row++,col,"Carrier","%'.3f",demod->tune.freq); // RF carrier frequency
  
  // second LO frequency is negative of IF, i.e., a signal at +48 kHz
  // needs a second LO frequency of -48 kHz to bring it to zero
  if(Frontend.sdr.lock)
    wattron(w,A_UNDERLINE);
  pprintw(w,row++,col,"First LO","%'.3f",Frontend.sdr.frequency);
  wattroff(w,A_UNDERLINE);

  // Wink IF display if out of front end's range
  wattroff(w,A_UNDERLINE);
  if(-demod->tune.second_LO + demod->filter.min_IF < Frontend.sdr.min_IF)
    wattron(w,A_BLINK);
  if(-demod->tune.second_LO + demod->filter.max_IF > Frontend.sdr.max_IF)
    wattron(w,A_BLINK);
  
  pprintw(w,row++,col,"IF","%'.3f",-demod->tune.second_LO);
  wattroff(w,A_BLINK);
  
  pprintw(w,row++,col,"Filter low","%'+.0f",demod->filter.min_IF);
  pprintw(w,row++,col,"Filter high","%'+.0f",demod->filter.max_IF);
  
  if(!isnan(demod->tune.shift))
    pprintw(w,row++,col,"Shift","%'+.3f",demod->tune.shift);
  
  pprintw(w,row++,col,"FE filter low","%'+.0f",Frontend.sdr.min_IF);
  pprintw(w,row++,col,"FE filter high","%'+.0f",Frontend.sdr.max_IF);

  // Doppler info displayed only if active
  double dopp = demod->tune.doppler;
  if(dopp != 0){
    pprintw(w,row++,col,"Doppler","%'.3f",dopp);
    pprintw(w,row++,col,"Dop Rate, Hz/s","%'.3f",demod->tune.doppler_rate);
  }
  row++; // Blank line between frequency & band info
  display_info(w,row,col,demod); // moved to bottom of tuning window
  box(w,0,0);
  mvwaddstr(w,0,1,"Tuning Hz");
  // Highlight cursor for tuning step
  // A little messy because of the commas in the frequencies
  // They come from the ' option in the printf formats
  // tunestep is the log10 of the digit position (0 = units)
  int hcol;
  if(Control.step >= 0){
    hcol = Control.step + Control.step/3;
    hcol = -hcol;
  } else {
    hcol = -Control.step;
    hcol = 1 + hcol + (hcol-1)/3; // 1 for the decimal point, and extras if there were commas in more than 3 places
  }
  int mod_x,mod_y;
  mod_y = Control.item + 1;
  mod_x = 24 + hcol; // units in column 24
  mvwchgat(w,mod_y,mod_x,1,A_STANDOUT,0,NULL);
  wnoutrefresh(w);
}

// Imbed in tuning window
void display_info(WINDOW *w,int row,int col,struct demod const *demod){
  if(w == NULL)
    return;

  struct bandplan const *bp_low,*bp_high;
  bp_low = lookup_frequency(demod->tune.freq + demod->filter.min_IF);
  bp_high = lookup_frequency(demod->tune.freq + demod->filter.max_IF);
  // Make sure entire receiver passband is in the band
  if(bp_low != NULL && bp_high != NULL){
    if(bp_low)
      mvwaddstr(w,row++,col,bp_low->description);
    if(bp_high && bp_high != bp_low)
    mvwaddstr(w,row++,col,bp_high->description);    
  }
}
void display_filtering(WINDOW *w,struct demod const *demod){
  if(w == NULL)
    return;

  // Filter window values
  int row = 1;
  int col = 1;
  wmove(w,row,col);
  wclrtobot(w);
  pprintw(w,row++,col,"Fs in","%'d Hz",Frontend.sdr.samprate); // Nominal
  pprintw(w,row++,col,"Fs out","%'d Hz",demod->output.samprate);
  
  pprintw(w,row++,col,"Block Time","%'.1f ms",Blocktime);
  
  long long const N = Frontend.L + Frontend.M - 1;
  
  pprintw(w,row++,col,"FFT in","%'lld %c ",N,Frontend.sdr.isreal ? 'r' : 'c');
  
  if(Frontend.sdr.samprate)
    pprintw(w,row++,col,"FFT out","%'lld   ",N * demod->output.samprate / Frontend.sdr.samprate);
  
  // FFT bin size
  pprintw(w,row++,col,"Overlap","%'.3f %% ",100.*(Frontend.M - 1)/(float)N);
  pprintw(w,row++,col,"Freq bin","%'.3f Hz",(float)Frontend.sdr.samprate / N);
  
  float const beta = demod->filter.kaiser_beta;
  pprintw(w,row++,col,"Kaiser beta","%'.1f   ",beta);
  
  
#if 0 // Doesn't really give accurate results
  // Play with Kaiser window values
  // Formulas taken from On the Use of the I0-sinh Window for Spectrum Analysis
  // James F Kaiser & Ronald W Schafer
  // ieee transaction on accoustics feb 1980
  // Eq (7) attenuation of first sidelobe
  float const cos_theta_r = 0.217324; // cosine of the first solution of tan(x) = x [really]
  float atten = 20 * log10(sinh(beta) / (cos_theta_r * beta));
  
  pprintw(w,row++,col,"Sidelobes","%'.1f dB",-atten);
  
  //    float firstnull = (1/(2*M_PI)) * sqrtf(M_PI * M_PI + beta*beta); // Eqn (3) to first null
  float const transition = (2.0 / M_PI) * sqrtf(M_PI*M_PI + beta * beta);
  pprintw(w,row++,col,"transition","%'.1f Hz",transition * Frontend.sdr.samprate / (Frontend.M-1)); // Not N, apparently
#endif
  pprintw(w,row++,col,"Drops","%'llu",Block_drops);
  
  box(w,0,0);
  mvwaddstr(w,0,1,"Filtering");
  
  wnoutrefresh(w);
}
// Signal data window
void display_sig(WINDOW *w,struct demod const *demod){
  if(w == NULL)
    return;

  float const noise_bandwidth = fabsf(demod->filter.max_IF - demod->filter.min_IF);
  float sig_power = demod->sig.bb_power - noise_bandwidth * Frontend.n0;
  if(sig_power < 0)
    sig_power = 0;
  float ad_dB = power2dB(Frontend.sdr.output_level);
  float fe_gain_dB = 0;
  if(Frontend.sdr.gain > 0)
    fe_gain_dB = voltage2dB(Frontend.sdr.gain);

  int row = 1;
  int col = 1;
  wmove(w,row,col);
  wclrtobot(w);
  mvwaddstr(w,row++,col,Frontend.sdr.description);
  pprintw(w,row++,col,"A Gain","%02d+%02d+%02d dB   ",Frontend.sdr.lna_gain,
	    Frontend.sdr.mixer_gain,
	    Frontend.sdr.if_gain);

  pprintw(w,row++,col,"A/D","%'.1f dBFS ",ad_dB);
  pprintw(w,row++,col,"In Gain","%.1f dB   ",fe_gain_dB);
  pprintw(w,row++,col,"Input","%.1f dB   ",ad_dB - fe_gain_dB);
  pprintw(w,row++,col,"Baseband","%.1f dB   ",power2dB(demod->sig.bb_power));
  pprintw(w,row++,col,"N0","%.1f dB/Hz",power2dB(Frontend.n0));
  
  float sn0 = sig_power/Frontend.n0;
  pprintw(w,row++,col,"S/N0","%.1f dBHz ",power2dB(sn0));
  pprintw(w,row++,col,"NBW","%.1f dBHz ",power2dB(noise_bandwidth));
  pprintw(w,row++,col,"SNR","%.1f dB   ",power2dB(sn0/noise_bandwidth));
  pprintw(w,row++,col,"Gain","%.1lf dB   ",voltage2dB(demod->output.gain));
  pprintw(w,row++,col,"Output","%.1lf dBFS ",power2dB(demod->output.level));
  pprintw(w,row++,col,"Headroom","%.1f dBFS ",voltage2dB(demod->output.headroom));
  box(w,0,0);
  mvwaddstr(w,0,1,"Signal");
  wnoutrefresh(w);
}
void display_demodulator(WINDOW *w,struct demod const *demod){
  if(w == NULL)
    return;

  // Demodulator info
  wmove(w,0,0);
  wclrtobot(w);
  int row = 1;
  int col = 1;

  // Display only if used by current mode
  switch(demod->demod_type){
  case FM_DEMOD:
  case WFM_DEMOD:
    pprintw(w,row++,col,"Input SNR","%.1f dB",power2dB(demod->sig.snr));
    pprintw(w,row++,col,"Squelch open","%.1f dB",power2dB(demod->squelch_open));
    pprintw(w,row++,col,"Squelch close","%.1f dB",power2dB(demod->squelch_close));    
    pprintw(w,row++,col,"Offset","%'+.3f Hz",demod->sig.foffset);
    pprintw(w,row++,col,"Deviation","%.1f Hz",demod->fm.pdeviation);
    if(!isnan(demod->fm.tone_freq) && demod->fm.tone_freq != 0)
      pprintw(w,row++,col,"Tone squelch","%.1f Hz",demod->fm.tone_freq);
    if(!isnan(demod->fm.tone_deviation) && !isnan(demod->fm.tone_freq) && demod->fm.tone_freq != 0)
      pprintw(w,row++,col,"Tone dev","%.1f Hz",demod->fm.tone_deviation);
    if(demod->deemph.rate != 0){
      pprintw(w,row++,col,"Deemph tc","%.1f us",demod->deemph.rate);
      pprintw(w,row++,col,"Deemph gain","%.1f dB",demod->deemph.gain);
    }
    break;
  case LINEAR_DEMOD:
    if(!isnan(demod->linear.threshold) && demod->linear.threshold > 0)
      pprintw(w,row++,col,"Threshold","%.1f dB  ",voltage2dB(demod->linear.threshold));
    if(!isnan(demod->linear.recovery_rate) && demod->linear.recovery_rate > 0)
      pprintw(w,row++,col,"Recovery rate","%.1f dB/s",voltage2dB(demod->linear.recovery_rate));
    if(!isnan(demod->linear.hangtime))
      pprintw(w,row++,col,"Hang time","%.1f s   ",demod->linear.hangtime);
    
    if(demod->linear.pll){
      pprintw(w,row++,col,"PLL BW","%.1f Hz  ",demod->linear.loop_bw);
      pprintw(w,row++,col,"PLL SNR","%.1f dB  ",power2dB(demod->sig.snr));
      pprintw(w,row++,col,"Offset","%'+.3f Hz  ",demod->sig.foffset);
      pprintw(w,row++,col,"PLL Phase","%+.1f deg ",demod->linear.cphase*DEGPRA);
      pprintw(w,row++,col,"PLL Lock","%s     ",demod->linear.pll_lock ? "Yes" : "No");
      pprintw(w,row++,col,"Squelch open","%.1f dB  ",power2dB(demod->squelch_open));
      pprintw(w,row++,col,"Squelch close","%.1f dB  ",power2dB(demod->squelch_close));    
    }
    break;
  }

  if(!isnan(demod->tp1))
    pprintw(w,row++,col,"TP1","%+g",demod->tp1);

  if(!isnan(demod->tp2))
    pprintw(w,row++,col,"TP2","%+g",demod->tp2);

  box(w,0,0);
  mvwprintw(w,0,1,"%s demodulator",demod_name_from_type(demod->demod_type));
  wnoutrefresh(w);
}
void display_fe(WINDOW *w,struct demod const *demod){
  if(w == NULL)
    return;

  // front hardware status: sample rate, tcxo offset, I/Q offset and imbalance, gain settings
  
  int row = 1;
  int col = 1;
  wmove(w,row,col);
  wclrtobot(w);
  char tbuf[100];
  mvwaddstr(w,row++,col,format_gpstime(tbuf,sizeof(tbuf),Frontend.sdr.timestamp));
  mvwaddstr(w,row++,col,formatsock(&Frontend.input.metadata_source_address));
  mvwaddstr(w,row++,col,formatsock(&Frontend.input.metadata_dest_address));
  
  pprintw(w,row++,col,"stat pkts","%'llu",Frontend.input.metadata_packets);
  pprintw(w,row++,col,"ctl pkts","%'llu",Frontend.sdr.commands);

  mvwhline(w,row,0,0,1000);
  mvwaddstr(w,row++,1,"Front end data");  

  mvwaddstr(w,row++,col,formatsock(&Frontend.input.data_source_address));
  mvwaddstr(w,row++,col,formatsock(&Frontend.input.data_dest_address));
  pprintw(w,row++,col,"ssrc","%'u",Frontend.input.rtp.ssrc);
  pprintw(w,row++,col,"pkts","%'llu",Frontend.input.rtp.packets);
  pprintw(w,row++,col,"samples","%'llu",Frontend.input.samples);
  pprintw(w,row++,col,"drops","%'llu",Frontend.input.rtp.drops);
  pprintw(w,row++,col,"dupes","%'llu",Frontend.input.rtp.dupes);
  box(w,0,0);
  mvwaddstr(w,0,1,"Front end status");
  wnoutrefresh(w);
}
void display_output(WINDOW *w,struct demod const *demod){
  if(w == NULL)
    return;

  int row = 1;
  int col = 1;
  wmove(w,row,col);
  wclrtobot(w);
  mvwaddstr(w,row++,col,formatsock(&Metadata_source_address));
  mvwaddstr(w,row++,col,formatsock(&Metadata_dest_address));
  pprintw(w,row++,col,"stat pkts","%'llu",Metadata_packets);
  pprintw(w,row++,col,"ctl pkts","%'llu",Commands);

  mvwhline(w,row,0,0,1000);
  mvwaddstr(w,row++,1,"Output data");  

  mvwaddstr(w,row++,col,formatsock(&demod->output.data_source_address));
  mvwaddstr(w,row++,col,formatsock(&demod->output.data_dest_address));
  
  pprintw(w,row++,col,"ssrc","%'u",demod->output.rtp.ssrc);
  pprintw(w,row++,col,"pkts","%'ld",demod->output.rtp.packets);
  
  
  box(w,0,0);
  mvwaddstr(w,0,1,"Output status");
  wnoutrefresh(w);
}

void display_options(WINDOW *w,struct demod const *demod){
  if(w == NULL)
    return;

  // Demodulator options, can be set with mouse
  int row = 1;
  int col = 1;
  wmove(w,row,col);
  wclrtobot(w);
  if(demod->demod_type == FM_DEMOD){ // FM from status.h
    // No options
  } else if(demod->demod_type == WFM_DEMOD){
    // Mono/stereo are only options
    if(demod->output.channels == 1)
      wattron(w,A_UNDERLINE);
    mvwaddstr(w,row++,col,"Mono");    
    wattroff(w,A_UNDERLINE);
    
    if(demod->output.channels == 2)
      wattron(w,A_UNDERLINE);
    mvwaddstr(w,row++,col,"Stereo");    
    wattroff(w,A_UNDERLINE);
  } else if(demod->demod_type == LINEAR_DEMOD){
    if(demod->linear.env && demod->output.channels == 1)
      wattron(w,A_UNDERLINE);	
    mvwaddstr(w,row++,col,"Envelope");
    wattroff(w,A_UNDERLINE);
    
    if(demod->linear.env && demod->output.channels == 2)
      wattron(w,A_UNDERLINE);
    mvwaddstr(w,row++,col,"Linear+Envelope");
    wattroff(w,A_UNDERLINE);
    
    if(!demod->linear.env && demod->output.channels == 1)
      wattron(w,A_UNDERLINE);
    mvwaddstr(w,row++,col,"Linear");
    wattroff(w,A_UNDERLINE);
    
    if(!demod->linear.env && demod->output.channels == 2)
      wattron(w,A_UNDERLINE);
    mvwaddstr(w,row++,col,"I/Q");    
    wattroff(w,A_UNDERLINE);
    
    if(!demod->linear.pll)
      wattron(w,A_UNDERLINE);      
    mvwaddstr(w,row++,col,"PLL Off");
    wattroff(w,A_UNDERLINE);
    
    if(demod->linear.pll && !demod->linear.square)
      wattron(w,A_UNDERLINE);      
    mvwaddstr(w,row++,col,"PLL On");
    wattroff(w,A_UNDERLINE);
    
    if(demod->linear.pll && demod->linear.square)
      wattron(w,A_UNDERLINE);            
    mvwaddstr(w,row++,col,"PLL Square");
    wattroff(w,A_UNDERLINE);
    
    if(!demod->linear.agc)
      wattron(w,A_UNDERLINE);
    mvwaddstr(w,row++,col,"AGC Off");
    wattroff(w,A_UNDERLINE);     
    if(demod->linear.agc)
      wattron(w,A_UNDERLINE);
    mvwaddstr(w,row++,col,"AGC On");
    wattroff(w,A_UNDERLINE);
    
  }
  box(w,0,0);
  mvwaddstr(w,0,1,"Options");
  wnoutrefresh(w);
}

void display_modes(WINDOW *w,struct demod const *demod){
  if(w == NULL)
    return;

  // Display list of modes defined in modes.conf
  // These are now really presets that select a demodulator and parameters,
  // so they're no longer underlined
  // Can be selected with mouse
  int row = 1;
  int col = 1;
  int npresets = iniparser_getnsec(Mdict);

  for(int i=0;i<npresets;i++)
    mvwaddstr(w,row++,col,iniparser_getsecname(Mdict,i));
  box(w,0,0);
  mvwaddstr(w,0,1,"Presets");
  wnoutrefresh(w);
}

// Set first (front end tuner) oscillator
// Note: single precision floating point is not accurate enough at VHF and above
// demod->first_LO is NOT updated here!
// It is set by incoming status frames so this will take time
double set_first_LO(struct demod const * const demod,double first_LO){
  assert(demod != NULL);
  if(demod == NULL)
    return NAN;

  unsigned char packet[8192],*bp;
  memset(packet,0,sizeof(packet));
  bp = packet;
  *bp++ = 1; // Command
  Frontend.sdr.command_tag = random();
  encode_int32(&bp,COMMAND_TAG,Frontend.sdr.command_tag);
  encode_double(&bp,RADIO_FREQUENCY,first_LO);
  encode_eol(&bp);
  int len = bp - packet;
  send(Frontend.input.ctl_fd,packet,len,0);
  return first_LO;
}  
// Like mvwprintw, but right justify the formatted output on the line and overlay with
// a left-justified label
int pprintw(WINDOW *w,int y, int x, char const *label, char const *fmt,...){
  int maxy __attribute__((unused)); // needed for getmaxyx
  int maxx;
  getmaxyx(w,maxy,maxx);

  // Format the variables
  va_list ap;
  va_start(ap,fmt);
  char result[maxx+1];
  int const r = vsnprintf(result,sizeof(result)-1,fmt,ap);
  va_end(ap);
  
  if(r == -1)
    return -1;

  result[sizeof(result)-1] = '\0'; // Ensure it's terminated

  // vsnprintf returns character count that *would* be written without limits
  // We want the actual length
  int const len = strlen(result);

  int vstart = maxx - 2 - len; // Allow an extra space for right side of box
  if(vstart < 0)
    vstart = 0; // Too long, truncate on right

  wmove(w,y,x);
  wclrtoeol(w);
  mvwaddstr(w,y,x+vstart,result);
  mvwaddstr(w,y,x,label);
  return 0;
}
