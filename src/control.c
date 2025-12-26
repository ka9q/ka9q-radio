// Interactive program to send commands and display internal state of 'radiod'
// Why are user interfaces always the biggest, ugliest and buggiest part of any program?
// Written as one big polling loop because ncurses is **not** thread safe

// Copyright 2017-2024 Phil Karn, KA9Q
// Major revisions fall 2020, 2023 (really continuous revisions!)

#define _GNU_SOURCE 1
#include <assert.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <unistd.h>
#include <stdbool.h>
#include <limits.h>
#include <string.h>
#if defined(linux)
#include <bsd/string.h>
#include <bsd/stdlib.h> // for arc4random()
#endif
#include <math.h>
#include <complex.h>
#undef I
#include <poll.h>
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
#include <sysexits.h>
#include <errno.h>
#include <fcntl.h>

#include "avahi.h"
#include "misc.h"
#include "filter.h"
#include "multicast.h"
#include "bandplan.h"
#include "status.h"
#include "radio.h"
#include "config.h"

static int const DEFAULT_IP_TOS = 48;
static int const DEFAULT_MCAST_TTL = 1; // LAN only, no routers
static double Refresh_rate = 0.25;
static char Locale[256] = "en_US.UTF-8";
static char const *Presets_file = "presets.conf"; // make configurable!
static dictionary *Pdict;
struct frontend Frontend;
struct sockaddr Metadata_source_socket;      // Source of metadata

#define TABLE_SIZE (1000)
int Mcast_ttl = DEFAULT_MCAST_TTL; // Should probably be settable from the command line
int IP_tos = DEFAULT_IP_TOS;
double Blocktime; // Now in seconds, not milliseconds
int Overlap;
int Output_fd,Status_fd;
const char *App_path;
int Verbose;

static struct control {
  int item;
  bool lock;
  int step;
} Control;


static struct {
  double noise_bandwidth;
  double sig_power;
  double sn0;
  double snr;
  int64_t pll_start_time;
  double pll_start_phase;
} Local;

static int send_poll(int ssrc);
static int pprintw(WINDOW *w,int y, int x, char const *prefix, char const *fmt, ...);

static WINDOW *Tuning_win,*Sig_win,*Filtering_win,*Demodulator_win,
  *Options_win,*Presets_win,*Debug_win,*Input_win,
  *Output_win;

static void display_tuning(WINDOW *tuning,struct channel const *chan);
static void display_info(WINDOW *w,int row,int col,struct channel const *chan);
static void display_filtering(WINDOW *filtering,struct channel const *chan);
static void display_sig(WINDOW *sig,struct channel const *chan);
static void display_demodulator(WINDOW *demodulator,struct channel const *chan);
static void display_options(WINDOW *options,struct channel const *chan);
static void display_presets(WINDOW *modes,struct channel const *chan);
static void display_input(WINDOW *input,struct channel const *chan);
static void display_output(WINDOW *output,struct channel const *chan);
static int process_keyboard(struct channel *,uint8_t **bpp,int c);
static void process_mouse(struct channel *chan,uint8_t **bpp);
static bool for_us(uint8_t const *buffer,size_t length,uint32_t ssrc);
static int init_demod(struct channel *chan);

// Fill in set of locally generated variables from channel structure
static void gen_locals(struct channel *chan){
  Local.noise_bandwidth = fabs(chan->filter.max_IF - chan->filter.min_IF);
  Local.sig_power = chan->sig.bb_power - Local.noise_bandwidth * chan->sig.n0; // signal power only (no noise)
  if(Local.sig_power < 0)
    Local.sig_power = 0; // Avoid log(-x) = nan
  Local.sn0 = Local.sig_power/chan->sig.n0;
  Local.snr = power2dB(Local.sn0/Local.noise_bandwidth);
}

// Pop up a temporary window with the contents of a file in the
// library directory (usually /usr/local/share/ka9q-radio/)
// then wait for a single keyboard character to clear it
static void popup(char const *filename){
  char fname[PATH_MAX];
  if (dist_path(fname,sizeof(fname),filename) == -1)
    return;
  FILE * const fp = fopen(fname,"r");
  if(fp == NULL)
    return;
  // Determine size of box
  int rows=0, cols=0;
  char *line = NULL;
  size_t maxcols = 0;
  while(getline(&line,&maxcols,fp) > 0){
    chomp(line);
    rows++;
    if((int)strlen(line) > cols)
      cols = (int)strlen(line); // Longest line
  }
  rewind(fp);

  // Allow room for box
  WINDOW * const pop = newwin(rows+2,cols+2,0,0);
  box(pop,0,0);
  int row = 1; // Start inside box
  while(getline(&line,&maxcols,fp) > 0){
    chomp(line);
    mvwaddstr(pop,row++,1,line);
  }
  fclose(fp);
  FREE(line);
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
static void getentry(char const *prompt,char *response,int len){
  int boxwidth = (int)strlen(prompt) + len;
  WINDOW * const pwin = newwin(5,boxwidth+2,0,0);
  box(pwin,0,0);
  mvwaddstr(pwin,1,1,prompt);
  wrefresh(pwin);
  echo();
  timeout(-1);
  // Manpage for wgetnstr doesn't say whether a terminating
  // null is stashed. Hard to believe it isn't, but this is to be sure
  memset(response,0,len);
  int r = wgetnstr(pwin,response,len);
  if(r != OK)
    memset(response,0,len); // Zero out the read buffer
  chomp(response);
  timeout(0);
  noecho();
  werase(pwin);
  wrefresh(pwin);
  delwin(pwin);
}

static FILE *Tty;
static SCREEN *Term;

static void display_cleanup(void){
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
static void adjust_item(struct channel *chan,uint8_t **bpp,int direction){
  double tunestep = pow(10., (double)Control.step);

  if(!direction)
    tunestep = - tunestep;

  switch(Control.item){
  case 0: // Carrier frequency
    if(!Frequency_lock){ // Ignore if locked
      chan->tune.freq += tunestep;
      encode_double(bpp,RADIO_FREQUENCY,chan->tune.freq);
    }
    break;
  case 1: // First LO
    if(Control.lock) // Tuner is locked, don't change it
      break;
    // Send via radiod
    encode_float(bpp,FIRST_LO_FREQUENCY,Frontend.frequency+tunestep);
    break;
  case 2: // IF (not implemented)
    break;
  case 3: // Filter low edge (hertz rather than millihertz)
    {
      double const x = min(chan->filter.max_IF,chan->filter.min_IF + (double)tunestep * 1000);
      chan->filter.min_IF = x;
      encode_float(bpp,LOW_EDGE,x);
    }
    break;
  case 4: // Filter high edge
    {
      double const x = max(chan->filter.min_IF,chan->filter.max_IF + (double)tunestep * 1000);
      chan->filter.max_IF = x;
      encode_float(bpp,HIGH_EDGE,x);
    }
    break;
  case 5: // Post-detection audio frequency shift
    chan->tune.shift += tunestep;
    encode_double(bpp,SHIFT_FREQUENCY,chan->tune.shift);
    break;

  }
}

// It seems better to just use the Griffin application to turn knob events into keystrokes or mouse events
static void adjust_up(struct channel *chan,uint8_t **bpp){
  adjust_item(chan,bpp,1);
}
static void adjust_down(struct channel *chan,uint8_t **bpp){
  adjust_item(chan,bpp,0);
}
static void toggle_lock(void){
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
static struct windef {
  WINDOW **w;
  int rows;
  int cols;
} Windefs[] = {
  {&Tuning_win, 20, 30},
  {&Options_win, 20, 12},
  //  {&Presets_win,Npresets+2,9}, // Npresets is not a static initializer
  {&Presets_win,20,9},
  {&Sig_win,20,25},
  {&Demodulator_win,20,26},
  {&Filtering_win,20,22},
  {&Input_win,20,60},
  {&Output_win,20,60},
};
#define NWINS ((int)(sizeof(Windefs) / sizeof(Windefs[0])))

static void setup_windows(void){
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
  // Specially set up debug window
  // Minimum of 45 cols for debug window, otherwise go to next row
  if(col + 45 > COLS){
    row += maxrows;
    col = 0;
  }
  if(row < LINES && col < COLS)
    Debug_win = newwin(LINES - row,COLS-col,row,col); // Only if room is left
  // A message from our sponsor...
  scrollok(Debug_win,TRUE); // This one scrolls so it can be written to with wprintw(...\n)
  wprintw(Debug_win,"KA9Q-radio %s last modified %s\n",__FILE__,__TIMESTAMP__);
  wprintw(Debug_win,"Copyright 2024, Phil Karn, KA9Q. May be used under the terms of the GNU Public License\n");
}

// Comparison for sorting by SSRC
static int chan_compare(void const *a,void const *b){
  struct channel const *da = *(struct channel **)a;
  struct channel const *db = *(struct channel **)b;
  if(da->output.rtp.ssrc < db->output.rtp.ssrc){
    return -1;
  }
  if(da->output.rtp.ssrc > db->output.rtp.ssrc){
    return +1;
  }
  return 0;
}


static uint32_t Ssrc = 0;

// Thread to display receiver state, updated at 10Hz by default
// Uses the ancient ncurses text windowing library
// Also services keyboard, mouse and tuning knob, if present
int main(int argc,char *argv[]){
  App_path = argv[0];
  {
    int c;
    while((c = getopt(argc,argv,"vVs:r:")) != -1){
      switch(c){
      case 'V':
	VERSION();
	exit(EX_OK);
      case 'v':
	Verbose++;
	break;
      case 's':
	Ssrc = atoi(optarg); // Send to specific SSRC
	break;
      case 'r':
	Refresh_rate = strtod(optarg,NULL);
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
  char const *target = argc > optind ? argv[optind] : NULL;


  if(target == NULL){
    long entry = 0;
    struct service_tab table[TABLE_SIZE];
    char *line = NULL;
    size_t linesize = 0;

    while(true){
      // Use avahi browser to find a radiod instance to control
      fprintf(stdout,"Scanning for radiod instances...\n");

      int radiod_count = avahi_browse(table,TABLE_SIZE,"_ka9q-ctl._udp"); // Returns list in global when cache is emptied
      if(radiod_count == 0){
	fprintf(stdout,"No radiod instances or Avahi not running; hit return to retry or re-run and specify control channel\n");
	if(getline(&line,&linesize,stdin) < 0){
	  fprintf(stdout,"EOF on input\n");
	  exit(EX_USAGE);
	}
	continue;
      }

      if(radiod_count == 1){
	// Only one, use it
	fprintf(stdout,"Using %s (%s)\n",table[entry].name,table[entry].dns_name);
	break;
      } else {
	for(int i=0; i < radiod_count; i++)
	  fprintf(stdout,"%d: %s (%s)\n",i,table[i].name,table[i].dns_name);
	fprintf(stdout,"Select index: ");
	fflush(stdout);
	if(getline(&line,&linesize,stdin) <= 0){
	  fprintf(stdout,"EOF on input\n");
	  FREE(line);
	  exit(EX_USAGE);
	}
	char *endptr = NULL;
	entry = strtol(line,&endptr,0);

	if(line == endptr)
	  continue; // Go back and do it again
	if(entry < 0 || entry >= radiod_count){
	  fprintf(stdout,"Index %ld out of range, try again\n",entry);
	  continue;
	}
	fprintf(stdout,"Selected: %ld\n",entry);
	break;
      }
    }
    FREE(line);
    fprintf(stdout,"%s (%s):\n",table[entry].name,table[entry].dns_name);
    struct addrinfo *results = NULL;
    struct addrinfo hints;
    memset(&hints,0,sizeof(hints));
    hints.ai_family = AF_INET; // IPv4 for now
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    hints.ai_flags = AI_ADDRCONFIG | AI_NUMERICHOST | AI_NUMERICSERV;
    int const ecode = getaddrinfo(table[entry].address,table[entry].port,&hints,&results);
    if(ecode != 0){
      fprintf(stdout,"getaddrinfo: %s\n",gai_strerror(ecode));
      exit(EX_IOERR);
    }
    // Use first entry on list -- much simpler
    // I previously tried each entry in turn until one succeeded, but with UDP sockets and
    // flags set to only return supported addresses, how could any of them fail?
    memcpy(&Frontend.metadata_dest_socket,results->ai_addr,sizeof(Frontend.metadata_dest_socket));
    freeaddrinfo(results); results = NULL;
    Status_fd = listen_mcast(NULL,&Frontend.metadata_dest_socket,table[entry].interface);
    Output_fd = output_mcast(&Frontend.metadata_dest_socket,NULL,Mcast_ttl,IP_tos);
    join_group(Output_fd,NULL,&Frontend.metadata_dest_socket,table[entry].interface);
  } else {
    // Use resolve_mcast to resolve a manually entered domain name, using default port and parsing possible interface
    char iface[1024] = {0}; // Multicast interface string
    resolve_mcast(target,&Frontend.metadata_dest_socket,DEFAULT_STAT_PORT,iface,sizeof(iface),0);
    Status_fd = listen_mcast(NULL,&Frontend.metadata_dest_socket,iface);
    Output_fd = output_mcast(&Frontend.metadata_dest_socket,iface,Mcast_ttl,IP_tos);
  }
  if(Status_fd < 0){
    fprintf(stderr,"Can't listen to mcast status channel: %s\n",strerror(errno));
    exit(EX_IOERR);
  }
  if(Output_fd < 0){
    fprintf(stdout,"can't create output socket: %s\n",strerror(errno));
    exit(EX_OSERR); // let systemd restart us
  }
  {
    // All reads from the status channel will have a timeout
    // Should this be configurable?
    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 100000; // 100k microsec = 100 millisec
    if(setsockopt(Status_fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout)) == -1)
      perror("setsock timeout");
  }
  char presetsfile_path[PATH_MAX];
  if (dist_path(presetsfile_path,sizeof(presetsfile_path),Presets_file) == -1) {
    fprintf(stderr,"Could not find mode file %s\n", Presets_file);
    exit(EX_NOINPUT);
  }
  Pdict = iniparser_load(presetsfile_path);
  if(Pdict == NULL){
    fprintf(stdout,"Can't load mode file %s\n",presetsfile_path);
    exit(EX_NOINPUT);
  }
  atexit(display_cleanup);

  struct channel **channels = NULL;
  int chan_count = 0;
  while(Ssrc == 0){
    // No channel specified; poll radiod for a list, sort and let user choose
    // If responses are lost or delayed and the user gets an incomplete list, just hit return
    // and we'll poll again. New entries will be added & existing entries will be updated
    // though any that disappear from radiod will remain on the list (not a big deal here)
    // The search exits after either a 100 ms timeout waiting for any incoming message OR 1 sec with no new channels seen
    // The second test is important when monitoring a status channel busy with 'control' polls or ka9q-web spectrum data
    send_poll(0xffffffff);
    // Read responses
    int const chan_max = 1024;
    if(channels == NULL)
      channels = (struct channel **)calloc(chan_max,sizeof(struct channel *));

    assert(channels != NULL);
    int64_t last_new_entry = gps_time_ns();
    while(chan_count < chan_max){
      struct sockaddr_storage source_socket;
      socklen_t ssize = sizeof(source_socket);
      uint8_t buffer[PKTSIZE];
      ssize_t length = recvfrom(Status_fd,buffer,sizeof(buffer),0,(struct sockaddr *)&source_socket,&ssize); // should not block
      if(length == -1 && errno == EAGAIN)
	break; // Timeout; we're done
      // Ignore our own command packets
      if(length < 2 || (enum pkt_type)buffer[0] != STATUS)
	continue;

      // What to do with the source addresses?
      memcpy(&Metadata_source_socket,&source_socket,sizeof(Metadata_source_socket));
      struct channel * const chan = calloc(1,sizeof(struct channel));
      assert(chan != NULL);
      init_demod(chan);
      decode_radio_status(&Frontend,chan,buffer+1,length-1);

      // Do we already have it?
      int i;
      for(i=0; i < chan_count; i++)
	if(channels[i]->output.rtp.ssrc == chan->output.rtp.ssrc)
	  break;
      if(i < chan_count){
	// Already in table, replace
	assert(channels[i] != NULL);
	FREE(channels[i]);
	channels[i] = chan;
	if(gps_time_ns() > last_new_entry + BILLION)
	  break; // Give up after 1 sec with no new channels
      } else {
	channels[chan_count++] = chan; // New one, add
	last_new_entry = gps_time_ns();
      }
    }
    qsort(channels,chan_count,sizeof(channels[0]),chan_compare);
    fprintf(stdout,"%13s %9s %10s %13s %5s %s\n","SSRC","preset","samprate","freq, Hz","SNR","output channel");
    uint32_t last_ssrc = 0;
    for(int i=0; i < chan_count;i++){
      struct channel *chan = channels[i];
      if(chan == NULL || chan->output.rtp.ssrc == last_ssrc) // Skip dupes
	continue;

      char const *ip_addr_string = formatsock(&chan->output.dest_socket,true);
      gen_locals(chan);
      if(chan->output.encoding == OPUS)
	fprintf(stdout,"%13u %9s %10s %'13.f %5.1f %s\n",chan->output.rtp.ssrc,chan->preset,"opus",chan->tune.freq,Local.snr,ip_addr_string);
      else
	fprintf(stdout,"%13u %9s %'10d %'13.f %5.1f %s\n",chan->output.rtp.ssrc,chan->preset,chan->output.samprate,chan->tune.freq,Local.snr,ip_addr_string);
      last_ssrc = chan->output.rtp.ssrc;
    }
    fprintf(stdout,"%d channels; choose SSRC, create new SSRC, or hit return to look for more: ",chan_count);
    fflush(stdout);
    char *line = NULL;
    size_t length = 0;
    if(getline(&line,&length,stdin) <= 0){
      fprintf(stdout,"EOF on input, exiting\n");
      FREE(line);
      exit(EX_USAGE);
    }
    size_t const n = strtol(line,NULL,0);
    FREE(line);
    if(n > 0)
      Ssrc = (uint32_t)n; // Will cause a break from this loop
  }
  // Free channel structures and pointer array, if they were used
  for(int i=0; i < chan_count; i++){
    if(channels[i] != NULL)
      FREE(channels[i]);
  }
  FREE(channels);

  struct channel Channel;
  struct channel *chan = &Channel;
  init_demod(chan);

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

  Frontend.frequency = Frontend.min_IF = Frontend.max_IF = NAN;

  /* Main loop:
     Send poll if we haven't received one in our refresh interval
     See if anything has arrived (use short timeout)
     If there's a response, update local status & repaint display windows
     Poll keyboard and process user commands

     Randomize polls over +/- 32 ms in case someone else is also polling
     This avoids possible synchronized back-to-back polls
     This is a common technique in multicast protocols (e.g., IGMP queries)
  */
  int const random_interval = 64 << 20; // power of 2 makes it easier for arc4random_uniform()

  int64_t now = gps_time_ns();
  int64_t next_radio_poll = now; // Immediate first poll
  bool screen_update_needed = false;
  for(;;){
    int64_t const radio_poll_interval = (int64_t)(Refresh_rate * BILLION); // Can change from the keyboard
    if(now >= next_radio_poll){
      // Time to poll radio
      send_poll(Ssrc);
#ifdef DEBUG_POLL
      wprintw(Debug_win,"poll sent %lld\n",now);
#endif
      // Retransmit after 1/10 sec if no response
      next_radio_poll = now + radio_poll_interval + arc4random_uniform(random_interval) - random_interval/2;
    }
    // Poll the input socket
    // This paces keyboard polling so wait no more than 100 ms, even for long refresh intervals
    int const recv_timeout = BILLION/10;
    int64_t start_of_recv_poll = now;
    uint8_t buffer[PKTSIZE];
    ssize_t length = 0;
    do {
      now = gps_time_ns();
      // Message from the radio program (or some transcoders)
      struct sockaddr_storage source_socket;
      socklen_t ssize = sizeof(source_socket);
      length = recvfrom(Status_fd,buffer,sizeof(buffer),0,(struct sockaddr *)&source_socket,&ssize); // should not block
      // Ignore our own command packets and responses to other SSIDs
      if(length < 2 || (enum pkt_type)buffer[0] != STATUS || !for_us(buffer+1,length-1,Ssrc))
	continue; // Can include a timeout

      // Process only if it's a response to our SSRC
      memcpy(&Metadata_source_socket,&source_socket,sizeof(Metadata_source_socket));
      screen_update_needed = true;
#ifdef DEBUG_POLL
      wprintw(Debug_win,"got response length %d\n",length);
#endif
      decode_radio_status(&Frontend,chan,buffer+1,length-1);
      gen_locals(chan);

      // Postpone next poll to specified interval
      next_radio_poll = now + radio_poll_interval + arc4random_uniform(random_interval) - random_interval/2;
      if(Blocktime == 0 && Frontend.samprate != 0)
	Blocktime = Frontend.L / Frontend.samprate; // Set the first time
    } while(now < start_of_recv_poll + recv_timeout);
    // Set up command buffer in case we want to change something
    uint8_t cmdbuffer[PKTSIZE];
    uint8_t *bp = cmdbuffer;
    *bp++ = CMD; // Command

    // Poll keyboard and mouse
    int const c = getch();
    if(c == KEY_MOUSE){
      process_mouse(chan,&bp);
      screen_update_needed = true;
    } else if(c != ERR) {
      screen_update_needed = true;
      if(process_keyboard(chan,&bp,c) == -1)
	goto quit;
    }
    // Any commands to send?
    if(bp > cmdbuffer+1){
      // Yes
      assert(Ssrc != 0);
      encode_int(&bp,OUTPUT_SSRC,Ssrc); // Specific SSRC
      encode_int(&bp,COMMAND_TAG,arc4random()); // Append a command tag
      encode_eol(&bp);
      long const command_len = bp - cmdbuffer;
#ifdef DEBUG_POLL
      wprintw(Debug_win,"sent command len %ld\n",command_len);
      screen_update_needed = true; // show local change right away
#endif
      if(sendto(Output_fd, cmdbuffer, command_len, 0, &Frontend.metadata_dest_socket,sizeof Frontend.metadata_dest_socket) != command_len){
	wprintw(Debug_win,"command send error: %s\n",strerror(errno));
	screen_update_needed = true; // show local change right away
      }
      // This will elicit an answer, defer the next poll
      next_radio_poll = now + radio_poll_interval + arc4random_uniform(random_interval) - random_interval/2;
    }
    if(screen_update_needed){
      // update display windows
      display_tuning(Tuning_win,chan);
      display_filtering(Filtering_win,chan);
      display_sig(Sig_win,chan);
      display_demodulator(Demodulator_win,chan);
      display_options(Options_win,chan);
      display_presets(Presets_win,chan);
      display_input(Input_win,chan);
      display_output(Output_win,chan);

      if(Debug_win != NULL){
	touchwin(Debug_win); // since we're not redrawing it every cycle
	wnoutrefresh(Debug_win);
      }
      doupdate();      // Update the screen right before we pause
      screen_update_needed = false;
    }
  }
 quit:;
  endwin();
  set_term(NULL);
  if(Term != NULL)
    delscreen(Term);
#if 0 // double free error, not really needed anyway
  if(Tty != NULL)
    fclose(Tty);
#endif
  exit(EX_OK);
}

#define Entry_width (15)

static int process_keyboard(struct channel *chan,uint8_t **bpp,int c){
  // Look for keyboard and mouse events

  switch(c){
  case ERR:
    break;
  case KEY_RESIZE:
    setup_windows();
    break;
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
    adjust_up(chan,bpp);
    break;
  case KEY_DOWN:      // Decrease whatever we're tuning
    adjust_down(chan,bpp);
    break;
  case '\f':  // Screen repaint (formfeed, aka control-L)
    clearok(curscr,TRUE);
    break;
  case 'C':
    {
      char str[Entry_width];
      getentry("Spectrum crossover, Hz: ",str,sizeof(str));
      double crossover = fabs(parse_frequency(str,false));
      chan->spectrum.crossover = crossover;
      encode_float(bpp,CROSSOVER,crossover);
    }
    break;
  case 'S':
    {
      char str[Entry_width];
      getentry("Output sample rate, Hz: ",str,sizeof(str));
      double samprate = parse_frequency(str,false);
      if(samprate < 100)  // Minimum sample rate is 200 Hz for usual params (20 ms block, overlap = 20%)
	samprate *= 1000; // Assume the user meant kHz
      chan->output.samprate = (int)round(samprate);
      encode_int(bpp,OUTPUT_SAMPRATE,chan->output.samprate);
    }
    break;
  case 's': // Squelch threshold for current mode
    {
      char str[Entry_width],*ptr;
      getentry("Squelch SNR: ",str,sizeof(str));
      double const x = strtod(str,&ptr);
      if(ptr != str && isfinite(x)){
	encode_float(bpp,SQUELCH_OPEN,x);
	encode_float(bpp,SQUELCH_CLOSE,x - 1); // Make this a separate command
      }
    }
    break;
  case 'T': // Hang time, s (always taken as positive)
    {
      char str[Entry_width],*ptr;
      getentry("Hang time, s: ",str,sizeof(str));
      double const x = fabs(strtod(str,&ptr));
      if(ptr != str && isfinite(x))
	encode_float(bpp,AGC_HANGTIME,x);
    }
    break;
  case 'P': // PLL loop bandwidth, always positive
    {
      char str[Entry_width],*ptr;
      getentry("PLL loop bandwidth, Hz: ",str,sizeof(str));
      double const x = fabs(strtod(str,&ptr));
      if(ptr != str && isfinite(x))
	encode_float(bpp,PLL_BW,x);
    }
    break;
  case 'L': // AGC threshold, dB relative to headroom
    {
      char str[Entry_width],*ptr;
      getentry("AGC threshold, dB: ",str,sizeof(str));
      double const x = strtod(str,&ptr);
      if(ptr != str && isfinite(x))
	encode_float(bpp,AGC_THRESHOLD,x);
    }
    break;
  case 'R': // Recovery rate, dB/s (always taken as positive)
    {
      char str[Entry_width],*ptr;
      getentry("Recovery rate, dB/s: ",str,sizeof(str));
      double const x = fabs(strtod(str,&ptr));
      if(ptr != str && isfinite(x))
	encode_float(bpp,AGC_RECOVERY_RATE,x);
    }
    break;
  case 'H': // Target AGC output level (headroom), dB, taken as negative
    {
      char str[Entry_width],*ptr;
      getentry("Headroom, dB: ",str,sizeof(str));
      double const x = -fabs(strtod(str,&ptr));
      if(ptr != str && isfinite(x))
	encode_float(bpp,HEADROOM,x);
    }
    break;
  case 'G': // Manually set front end gain, dB (positive or negative)
    {
      char str[Entry_width],*ptr;
      getentry("RF Gain, dB: ",str,sizeof(str));
      double const x = strtod(str,&ptr);
      if(ptr != str && isfinite(x)){
	encode_float(bpp,RF_GAIN,x);
      }
    }
    break;
  case 'A': // Manually set front end attenuation, dB (positive or negative)
    {
      char str[Entry_width],*ptr;
      getentry("RF Atten, dB: ",str,sizeof(str));
      double const x = fabs(strtod(str,&ptr));
      if(ptr != str && isfinite(x)){
	encode_float(bpp,RF_ATTEN,x);
      }
    }
    break;
  case 'b':
    {
      char str[Entry_width],*ptr;
      getentry("Opus bitrate, bit/sec (0=auto): ",str,sizeof(str));
      long x = labs(strtol(str,&ptr,0));
      if(ptr != str){
	if(x < 510)
	  x *= 1000;
	encode_int(bpp,OPUS_BIT_RATE,(int)x);
      }
    }
    break;
  case 'B':
    {
      char str[Entry_width],*ptr;
      getentry("Packet buffering, blocks (0-4): ",str,sizeof(str));
      long x = labs(strtol(str,&ptr,0));
      if(ptr != str){
	if(x >= 0 && x <= 4)
	  encode_int(bpp,MINPACKET,(int)x);
      }
    }
    break;
  case 'g': // Manually set linear channel gain, dB (positive or negative)
    {
      char str[Entry_width],*ptr;
      getentry("Gain, dB: ",str,sizeof(str));
      double const x = strtod(str,&ptr);
      if(ptr != str && isfinite(x)){
	encode_float(bpp,GAIN,x);
	encode_byte(bpp,AGC_ENABLE,0); // Also done implicitly in radiod
      }
    }
    break;
  case 'r': // Poll/refresh rate
    {
      char str[Entry_width],*ptr;
      getentry("Refresh rate (s): ",str,sizeof(str));
      double const x = fabs(strtod(str,&ptr));
      if(ptr != str && isfinite(x))
	Refresh_rate = x;
    }
    break;
  case 'p':
  case 'm': // Manual mode preset
    {
      char str[Entry_width];
      char prompt[1024];
      snprintf(prompt,sizeof(prompt),"Mode/Preset [ ");
      int const nsec = iniparser_getnsec(Pdict);

      for(int i=0;i < nsec;i++){
	strlcat(prompt,iniparser_getsecname(Pdict,i),sizeof(prompt));
	strlcat(prompt," ",sizeof(prompt));
      }
      strlcat(prompt,"]: ",sizeof(prompt));
      getentry(prompt,str,sizeof(str));
      if(strlen(str) > 0)
	encode_string(bpp,PRESET,str,strlen(str));
    }
    break;
  case 'f':   // Tune to new radio frequency
    {
      char str[Entry_width];
      getentry("Carrier frequency: ",str,sizeof(str));
      if(strlen(str) > 0){
	double const x = fabs(parse_frequency(str,true)); // Handles funky forms like 147m435
	if(isfinite(x)){
	  chan->tune.freq = x;
	  encode_double(bpp,RADIO_FREQUENCY,chan->tune.freq);
	}
      }
    }
    break;
  case 'k': // Kaiser window parameter
    {
      char str[Entry_width],*ptr;
      getentry("Spectrum analyzer shape param β/σ: ",str,sizeof(str));
      double const b = strtod(str,&ptr);
      if(ptr != str && isfinite(b)){
	if(b < 0 || b >= 100){
	  beep(); // beyond limits
	} else {
	  encode_float(bpp,SPECTRUM_SHAPE,b);
	}
      }
    }
    break;
  case 'K': // Kaiser window parameter for filter2
    {
      char str[Entry_width],*ptr;
      getentry("Filter2 Kaiser window β: ",str,sizeof(str));
      double const b = strtod(str,&ptr);
      if(ptr != str && isfinite(b)){
	if(b < 0 || b >= 100){
	  beep(); // beyond limits
	} else {
	  encode_float(bpp,FILTER2_KAISER_BETA,b);
	}
      }
    }
    break;
  case 'W':
    {
      char str[Entry_width],*ptr;
      getentry("FFT Window type [0=Kaiser,1=rect,2=blackman,3=exact blackman,4=gaussian,5=hann,6=hamming: ",str,sizeof(str));
      int const b = strtol(str,&ptr,0);
      if(ptr != str){
	if(b >= N_WINDOW){
	  beep(); // beyond limits
	} else {
	  encode_int(bpp,WINDOW_TYPE,b);
	}
      }
    }
    break;
  case 'w':
    {
      char str[Entry_width],*ptr;
      getentry("Spectrum shape factor: ",str,sizeof(str));
      double const b = strtod(str,&ptr);
      if(ptr != str && isfinite(b)){
	if(b < 0 || b >= 100){
	  beep(); // beyond limits
	} else {
	  encode_float(bpp,SPECTRUM_SHAPE,b);
	}
      }
    }
    break;
  case 'o': // Set/clear option flags, most apply only to linear detector
    {
      char str[Entry_width];

      getentry("[isb pll square stereo mono agc], '!' prefix disables: ",str,sizeof(str));
      bool enable = true;
      if(strchr(str,'!') != NULL)
	enable = false;

      if(strcasestr(str,"mono") != NULL){
	encode_int(bpp,OUTPUT_CHANNELS,enable ? 1 : 2);
      } else if(strcasestr(str,"stereo") != NULL){
	encode_int(bpp,OUTPUT_CHANNELS,enable ? 2 : 1);
      } else if(strcasestr(str,"isb") != NULL){
	encode_byte(bpp,INDEPENDENT_SIDEBAND,enable);
      } else if(strcasestr(str,"pll") != NULL){
	encode_byte(bpp,PLL_ENABLE,enable);
      } else if(strcasestr(str,"square") != NULL){
	encode_byte(bpp,PLL_SQUARE,enable);
	if(enable)
	  encode_byte(bpp,PLL_ENABLE,enable);
      } else if(strcasestr(str,"agc") != NULL){
	encode_byte(bpp,AGC_ENABLE,enable);
      }
    }
    break;
  case 'O': // Set/clear aux option flags, mainly for testing
    {
      char str[Entry_width],*ptr;

      getentry("enter aux option number [0-63], ! disables: ",str,sizeof(str));
      bool enable = true;
      char *cp = strchr(str,'!');
      if(cp != NULL){
	enable = false;
	cp++;
      } else
	cp = str;
      long n = strtol(cp,&ptr,0);
      if(ptr != cp && n >= 0 && n < 64){
	if(enable)
	  encode_int64(bpp,SETOPTS,1LL<<n);
	else
	  encode_int64(bpp,CLEAROPTS,1LL<<n);
      }
    }
    break;
  case 'u':
    {
      char str[Entry_width],*ptr;
      getentry("Data channel status rate ",str,sizeof(str));
      long const b = strtol(str,&ptr,0);
      if(ptr != str && b >= 0)
	encode_int(bpp,STATUS_INTERVAL,(int)b);
    }
    break;
  case 'e':
    {
      char str[Entry_width];
      getentry("Output encoding [s16le s16be f32le f16le opus opus-voip]: ",str,sizeof(str));
      enum encoding e = parse_encoding(str);
      // "OPUS_VOIP" really means OPUS wih the OPUS_APPLICATION_VOIP option
      if(e == OPUS_VOIP){
	encode_int(bpp,OPUS_APPLICATION,OPUS_APPLICATION_VOIP);
	e = OPUS;
      } else if(e == OPUS){
	// Default is OPUS_APPLICATION_AUDIO
	encode_int(bpp,OPUS_APPLICATION,OPUS_APPLICATION_AUDIO);
      }
      if(e != NO_ENCODING)
	encode_int(bpp,OUTPUT_ENCODING,(uint8_t)e);
    }
    break;
  case 'F':
    {
      char str[Entry_width],*ptr;
      getentry("Filter2 blocksize (0-10): ",str,sizeof(str));
      long x = labs(strtol(str,&ptr,0));
      if(ptr != str)
	encode_int(bpp,FILTER2,(int)x);
    }
    break;
  case 'D':
    {
      char str[Entry_width];
      struct sockaddr_storage sock;
      getentry("Destination name/address: ",str,sizeof(str));
      resolve_mcast(str,&sock,DEFAULT_RTP_PORT,NULL,0,0);
      // Port is actually ignored by radiod, sets both data and status
      encode_socket(bpp,OUTPUT_DATA_DEST_SOCKET,&sock);
    }
    break;
  default:
    beep();
    break;
  } // switch
  return 0;
}

static void process_mouse(struct channel *chan,uint8_t **bpp){
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

    } else if(Presets_win && wmouse_trafo(Presets_win,&my,&mx,false)){
      // In the presets window?
      my--;
      if(my >= 0 && my < iniparser_getnsec(Pdict)){
	char const *preset = iniparser_getsecname(Pdict,my);
	encode_string(bpp,PRESET,preset,strlen(preset));
      }

    } else if(Options_win && wmouse_trafo(Options_win,&my,&mx,false)){
      // In the options window
      if(chan->demod_type == WFM_DEMOD){
	switch(my){
	case 1:
	  encode_int(bpp,OUTPUT_CHANNELS,1);
	  break;
	case 2:
	  encode_int(bpp,OUTPUT_CHANNELS,2);
	  break;
	case 3:
	  encode_int(bpp,SNR_SQUELCH,0);
	  break;
	case 4:
	  encode_int(bpp,SNR_SQUELCH,1);
	  break;
	}
      } else if(chan->demod_type == FM_DEMOD){
	switch(my){
	case 1:
	  encode_int(bpp,THRESH_EXTEND,0);
	  break;
	case 2:
	  encode_int(bpp,THRESH_EXTEND,1);
	  break;
	case 3:
	  encode_int(bpp,SNR_SQUELCH,0);
	  break;
	case 4:
	  encode_int(bpp,SNR_SQUELCH,1);
	  break;
	}
      } else if(chan->demod_type == LINEAR_DEMOD){
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
	case 10:
	  encode_int(bpp,SNR_SQUELCH,0);
	  break;
	case 11:
	  encode_int(bpp,SNR_SQUELCH,1);
	  break;
	}
      }
    }
  } // end of mouse processing
}

// Initialize a new, unused channel instance where fields might be non-zero
static int init_demod(struct channel *chan){
  if(chan == NULL)
    return -1;
  memset(chan,0,sizeof(*chan));
  chan->tune.second_LO = NAN;
  chan->tune.freq = chan->tune.shift = NAN;
  chan->filter.min_IF = chan->filter.max_IF = chan->filter.kaiser_beta = NAN;
  chan->spectrum.window_type = KAISER_WINDOW;
  chan->output.headroom = chan->linear.hangtime = chan->linear.recovery_rate = NAN;
  chan->sig.bb_power = chan->sig.foffset = NAN;
  chan->fm.pdeviation = chan->pll.cphase = NAN;
  chan->output.gain = NAN;
  chan->tp1 = chan->tp2 = NAN;
  return 0;
}

// Is response for us?
static bool for_us(uint8_t const *buffer,size_t length,uint32_t ssrc){
  uint8_t const *cp = buffer;

  while(cp < &buffer[length]){
    enum status_type const type = *cp++; // increment cp to length field

    if(type == EOL)
      break; // end of list, no length

    int optlen = *cp++;
    if(optlen & 0x80){
      // length is >= 128 bytes; fetch actual length from next N bytes, where N is low 7 bits of optlen
      int length_of_length = optlen & 0x7f;
      optlen = 0;
      while(length_of_length > 0){
	optlen <<= 8;
	optlen |= *cp++;
	length_of_length--;
      }
    }
    if(cp + optlen >= buffer + length)
      break; // invalid length; we can't continue to scan

    switch(type){
    case EOL: // Shouldn't get here
      goto done;
    case OUTPUT_SSRC: // If we've specified a SSRC, it must match it
      if(ssrc != 0){
	if(decode_int32(cp,optlen) == ssrc)
	  return true; // For us
	return false; // For someone else
      }
      break;
    default:
      break; // Ignore on this pass
    }
    cp += optlen;
  }
 done:;
  return false; // not specified, so not for us
}


static void display_tuning(WINDOW *w,struct channel const *chan){
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
  pprintw(w,row++,col,"Carrier","%'.3lf",chan->tune.freq); // RF carrier frequency

  // second LO frequency is negative of IF, i.e., a signal at +48 kHz
  // needs a second LO frequency of -48 kHz to bring it to zero
  if(Frontend.lock)
    wattron(w,A_UNDERLINE);
  pprintw(w,row++,col,"First LO","%'.3lf",Frontend.frequency);
  wattroff(w,A_UNDERLINE);

  // Wink IF display if out of front end's range
  wattroff(w,A_UNDERLINE);
  if(-chan->tune.second_LO + chan->filter.min_IF < Frontend.min_IF)
    wattron(w,A_BLINK);
  if(-chan->tune.second_LO + chan->filter.max_IF > Frontend.max_IF)
    wattron(w,A_BLINK);

  pprintw(w,row++,col,"IF","%'.3lf",-chan->tune.second_LO);
  wattroff(w,A_BLINK);

  pprintw(w,row++,col,"Filter low","%'+.0f",chan->filter.min_IF);
  pprintw(w,row++,col,"Filter high","%'+.0f",chan->filter.max_IF);

  if(!isnan(chan->tune.shift))
    pprintw(w,row++,col,"Shift","%'+.3lf",chan->tune.shift);

  pprintw(w,row++,col,"FE filter low","%'+.0f",Frontend.min_IF);
  pprintw(w,row++,col,"FE filter high","%'+.0f",Frontend.max_IF);

  // Doppler info displayed only if active
  double const dopp = chan->tune.doppler;
  if(dopp != 0){
    pprintw(w,row++,col,"Doppler","%'.3lf",dopp);
    pprintw(w,row++,col,"Dop Rate, Hz/s","%'.3lf",chan->tune.doppler_rate);
  }
  row++; // Blank line between frequency & band info
  display_info(w,row,col,chan); // moved to bottom of tuning window
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
static void display_info(WINDOW *w,int row,int col,struct channel const *chan){
  if(w == NULL)
    return;

  struct bandplan const *bp_low,*bp_high;
  bp_low = lookup_frequency(chan->tune.freq + chan->filter.min_IF + 1.0);
  bp_high = lookup_frequency(chan->tune.freq + chan->filter.max_IF - 1.0);
  // Make sure entire receiver passband is in the band
  if(bp_low != NULL && bp_high != NULL){
    if(bp_low)
      mvwaddstr(w,row++,col,bp_low->description);
    if(bp_high && bp_high != bp_low)
    mvwaddstr(w,row++,col,bp_high->description);
  }
}
static void display_filtering(WINDOW *w,struct channel const *chan){
  if(w == NULL)
    return;

  // Filter window values
  int row = 1;
  int col = 1;
  wmove(w,row,col);
  wclrtobot(w);



  pprintw(w,row++,col,"Block Time","%'.1f ms",1000*Blocktime);
  pprintw(w,row++,col,"Block rate","%'.3f Hz",1./Blocktime); // Just the block rate

  int64_t const N = Frontend.L + Frontend.M - 1;

  pprintw(w,row++,col,"FFT in","%'lld %c ",N,Frontend.isreal ? 'r' : 'c');

  if(Frontend.samprate != 0 && chan->output.samprate != 0){
    int fftout = (int)round(N * chan->output.samprate / Frontend.samprate);
    pprintw(w,row++,col,"FFT out","%'d c ",fftout);
  }

  Overlap = 1 + Frontend.L / (Frontend.M - 1); // recreate original overlap parameter
  pprintw(w,row++,col,"Overlap","%.1lf %% ",100./Overlap);
  pprintw(w,row++,col,"Bin width","%'.3lf Hz",Frontend.samprate / N);

  double const beta = chan->filter.kaiser_beta;
  if(isfinite(beta))
    pprintw(w,row++,col,"Kaiser β","%'.1lf   ",beta);


#if 0 // Doesn't really give accurate results
  // Play with Kaiser window values
  // Formulas taken from On the Use of the I0-sinh Window for Spectrum Analysis
  // James F Kaiser & Ronald W Schafer
  // ieee transaction on acoustics feb 1980
  // Eq (7) attenuation of first sidelobe
  double const cos_theta_r = 0.217324; // cosine of the first solution of tan(x) = x [really]
  double atten = 20 * log10(sinh(beta) / (cos_theta_r * beta));
  pprintw(w,row++,col,"Sidelobes","%'.1lf dB",-atten);

  double firstnull = (1/(2*M_PI)) * sqrt(M_PI * M_PI + beta*beta); // Eqn (3) to first null
  double const transition = (2.0 / M_PI) * sqrt(M_PI*M_PI + beta * beta);
  pprintw(w,row++,col,"first null","%'.1lf Hz",0.5 * transition * Frontend.samprate / (Frontend.M-1)); // Not N, apparently
  //  pprintw(w,row++,col,"first null","%'.1lf Hz",firstnull * 1. / Blocktime);
#endif

  pprintw(w,row++,col,"Drops","%'llu   ",chan->filter.out.block_drops);
  if(chan->filter2.blocking > 0){
      mvwhline(w,row,0,0,1000);
      mvwaddstr(w,row++,1,"Filter2");
      int L = chan->filter2.in.ilen;
      int M = chan->filter2.in.impulse_length;
      int N = L+M > 0 ? L + M - 1 : 0;

      double bt = 1000. * Blocktime * chan->filter2.blocking;
      pprintw(w,row++,col,"Block Time","%.1lf ms",bt);
      pprintw(w,row++,col,"Block rate","%.3lf Hz",1000./bt);
      pprintw(w,row++,col,"FFT","%u c ",N);
      double overlap = 1 + (double)L / (M-1);
      pprintw(w,row++,col,"Overlap","%.2f %% ",100./overlap);
      pprintw(w,row++,col,"Bin width","%.3lf Hz",1000./(overlap*bt));
      pprintw(w,row++,col,"Kaiser β","%.1lf   ",chan->filter2.kaiser_beta);
  }
  box(w,0,0);
  mvwaddstr(w,0,1,"Filtering");

  wnoutrefresh(w);
}
// Signal data window
static void display_sig(WINDOW *w,struct channel const *chan){
  if(w == NULL)
    return;

  double const noise_bandwidth = fabs(chan->filter.max_IF - chan->filter.min_IF);
  double sig_power = chan->sig.bb_power - noise_bandwidth * chan->sig.n0;
  if(sig_power < 0)
    sig_power = 0; // can happen due to smoothing

  int row = 1;
  int col = 1;
  wmove(w,row,col);
  wclrtobot(w);

  if(Frontend.lna_gain != 0 || Frontend.mixer_gain != 0 || Frontend.if_gain != 0)
    pprintw(w,row++,col,"A Gain","%02d+%02d+%02d dB   ",Frontend.lna_gain,
	    Frontend.mixer_gain,
	    Frontend.if_gain);

  // Calculate actual input power in dBm by subtracting net RF gain
  pprintw(w,row++,col,"Input","%.1lf dBm ",
	  power2dB(Frontend.if_power) - (Frontend.rf_gain - Frontend.rf_atten + Frontend.rf_level_cal));
  // These gain figures only affect the relative A/D input level in dBFS because an equal
  // amount of digital attenuation is applied to the A/D output to maintain unity gain
  pprintw(w,row++,col,"RF Gain","%.1lf dB  ",Frontend.rf_gain);
  pprintw(w,row++,col,"RF Atten","%.1lf dB  ",-Frontend.rf_atten);
  pprintw(w,row++,col,"RF lev cal","%.1lf dB  ",Frontend.rf_level_cal);
  pprintw(w,row++,col,"A/D","%.1lf dBFS",power2dB(Frontend.if_power));
  pprintw(w,row++,col,"Gain offset","%.1lf dB  ",-(Frontend.rf_gain - Frontend.rf_atten + Frontend.rf_level_cal));
  if(!isnan(chan->sig.bb_power))
    pprintw(w,row++,col,"Baseband","%.1lf dBm ",power2dB(chan->sig.bb_power));
  if(!isnan(chan->sig.n0)){
     pprintw(w,row++,col,"N₀","%.1lf dBmJ",power2dB(chan->sig.n0));
     double temp = chan->sig.n0 / (1000 * BOLTZMANN); // 1000 converts from joules to millijoules (for power in dBm)
     pprintw(w,row++,col,"N Temp","%.5lg K   ",temp);
     double nf = power2dB(1 + temp / 290); // convert to noise figure
     pprintw(w,row++,col,"NF","%.1lf dB  ",nf);
  }
  // Derived numbers
  if(!isnan(Local.sn0))
    pprintw(w,row++,col,"S/N₀","%.1lf dBHz",power2dB(Local.sn0));
  if(!isnan(Local.noise_bandwidth))
    pprintw(w,row++,col,"NBW","%.1lf dBHz",power2dB(Local.noise_bandwidth));
  if(!isnan(Local.snr))
    pprintw(w,row++,col,"S/N","%.1lf dB  ",Local.snr);
  if(!isnan(chan->output.gain) && chan->demod_type == LINEAR_DEMOD) // Only relevant in linear
    pprintw(w,row++,col,"Gain","%.1lf dB  ",voltage2dB(chan->output.gain));
  if(!isnan(chan->output.power))
    pprintw(w,row++,col,"Output","%.1lf dBFS",power2dB(chan->output.power));
  box(w,0,0);
  mvwaddstr(w,0,1,"Signal");
  wnoutrefresh(w);
}
static void display_demodulator(WINDOW *w,struct channel const *chan){
  if(w == NULL)
    return;

  // Demodulator info
  wmove(w,0,0);
  wclrtobot(w);
  int row = 1;
  int col = 1;

  // Display only if used by current mode
  switch(chan->demod_type){
  case FM_DEMOD:
  case WFM_DEMOD:
    pprintw(w,row++,col,"Input S/N","%.1lf dB",power2dB(chan->fm.snr));
    if(!isnan(chan->output.headroom))
      pprintw(w,row++,col,"Headroom","%.1lf dBFS ",voltage2dB(chan->output.headroom));
    pprintw(w,row++,col,"Squel open","%.1lf dB   ",power2dB(chan->squelch_open));
    pprintw(w,row++,col,"Squel close","%.1lf dB   ",power2dB(chan->squelch_close));
    pprintw(w,row++,col,"Offset","%'+.3lf Hz",chan->sig.foffset);
    pprintw(w,row++,col,"Deviation","%.1lf Hz",chan->fm.pdeviation);
    if(!isnan(chan->fm.tone_freq) && chan->fm.tone_freq != 0)
      pprintw(w,row++,col,"Tone squelch","%.1lf Hz",chan->fm.tone_freq);
    if(!isnan(chan->fm.tone_deviation) && !isnan(chan->fm.tone_freq) && chan->fm.tone_freq != 0)
      pprintw(w,row++,col,"Tone dev","%.1lf Hz",chan->fm.tone_deviation);
    if(chan->fm.rate != 0){
      pprintw(w,row++,col,"Deemph τ","%.1lf μs",chan->fm.rate);
      pprintw(w,row++,col,"Deemph gain","%.1lf dB",chan->fm.gain);
    }
    break;
  case LINEAR_DEMOD:
    if(!isnan(chan->output.headroom))
      pprintw(w,row++,col,"Headroom","%.1lf dBFS",voltage2dB(chan->output.headroom));
    pprintw(w,row++,col,"Squel open","%.1lf dB  ",power2dB(chan->squelch_open));
    pprintw(w,row++,col,"Squel close","%.1lf dB  ",power2dB(chan->squelch_close));

    if(!isnan(chan->linear.threshold) && chan->linear.threshold > 0)
      pprintw(w,row++,col,"AGC Threshold","%.1lf dB  ",voltage2dB(chan->linear.threshold));
    if(!isnan(chan->linear.recovery_rate) && chan->linear.recovery_rate > 0)
      pprintw(w,row++,col,"Recovery rate","%.1lf dB/s",voltage2dB(chan->linear.recovery_rate));
    if(!isnan(chan->linear.hangtime))
      pprintw(w,row++,col,"Hang time","%.1lf s   ",chan->linear.hangtime);

    if(chan->pll.enable){
      mvwhline(w,row,0,0,1000);
      mvwaddstr(w,row++,1,"PLL");
      mvwprintw(w,row++,col,"%-s",chan->pll.lock ? "Lock" : "Unlock");
      pprintw(w,row++,col,"BW","%.1lf Hz",chan->pll.loop_bw);
      pprintw(w,row++,col,"S/N","%.1lf dB",power2dB(chan->pll.snr));
      pprintw(w,row++,col,"Δf","%'+.3lf Hz",chan->sig.foffset);
      double phase = chan->pll.cphase * DEGPRA + 360 * chan->pll.rotations;

      pprintw(w,row++,col,"φ","%+.1lf °",chan->pll.cphase*DEGPRA);
      if(Local.pll_start_time == 0){
	Local.pll_start_time = gps_time_ns();
	Local.pll_start_phase = phase;
      }
      double delta_t = 1e-9 * (gps_time_ns() - Local.pll_start_time);
      double delta_ph = phase - Local.pll_start_phase;
      pprintw(w,row++,col,"ΔT","%.1lf s ",delta_t);
      pprintw(w,row++,col,"Δφ","%+.1lf °",delta_ph);
      pprintw(w,row++,col,"μ Δf/f","%lg",delta_ph / (360 * delta_t * chan->tune.freq));
    } else {
      Local.pll_start_time = 0;
    }
    break;
  case SPECT_DEMOD:
    pprintw(w,row++,col,"Bin width","%.0lf Hz",chan->spectrum.bin_bw);
    pprintw(w,row++,col,"Noise BW","%.1lf Hz",chan->spectrum.noise_bw);
    pprintw(w,row++,col,"Bins","%d   ",chan->spectrum.bin_count);
    pprintw(w,row++,col,"Crossover","%.0lf Hz",chan->spectrum.crossover);
    pprintw(w,row++,col,"FFT N","%d   ",chan->spectrum.fft_n);
    {
      char win_type[100];
      switch(chan->spectrum.window_type){
	case KAISER_WINDOW:
	  snprintf(win_type,sizeof win_type,"Kaiser β = %.1lf",chan->spectrum.shape);
	  break;
	case RECT_WINDOW:
	  snprintf(win_type,sizeof win_type,"rect");
	  break;
	case BLACKMAN_WINDOW:
	  snprintf(win_type,sizeof win_type,"Blackman");
	  break;
	case EXACT_BLACKMAN_WINDOW:
	  snprintf(win_type,sizeof win_type,"exact Blackman");
	  break;
	case GAUSSIAN_WINDOW:
	  snprintf(win_type,sizeof win_type,"Gaussian σ = %.1lf",chan->spectrum.shape);
	  break;
	case HANN_WINDOW:
	  snprintf(win_type,sizeof win_type,"Hann");
	  break;
	case HAMMING_WINDOW:
	  snprintf(win_type,sizeof win_type,"Hamming");
	  break;
	default:
	  snprintf(win_type,sizeof win_type,"unknown");
	  break;
      }
      pprintw(w,row++,col,"Window","%s",win_type);
    }
    if(chan->spectrum.bin_data != NULL)
      pprintw(w,row++,col,"Bin 0","%.1lf   ",chan->spectrum.bin_data[0]);
    break;
  default:
    break;
  }

  if(!isnan(chan->tp1))
    pprintw(w,row++,col,"TP1","%+lg",chan->tp1);

  if(!isnan(chan->tp2))
    pprintw(w,row++,col,"TP2","%+lg",chan->tp2);

  box(w,0,0);
  mvwprintw(w,0,1,"%s demodulator",demod_name_from_type(chan->demod_type));
  wnoutrefresh(w);
}

static void display_input(WINDOW *w,struct channel const *chan){
  if(w == NULL)
    return;

  int row = 1;
  int col = 1;
  wmove(w,row,col);
  wclrtobot(w);
  char tmp[100];
  pprintw(w,row++,col,"","%s",format_gpstime(tmp,sizeof(tmp),chan->clocktime));
  if(Frontend.samprate != 0){
    pprintw(w,row++,col,"Sample rate","%'.0lf Hz",Frontend.samprate); // Nominal
    pprintw(w,row++,col,"Uptime","%s",ftime(tmp,sizeof(tmp),(int64_t)(Frontend.samples/Frontend.samprate)));
    pprintw(w,row++,col,"Overranges","%'llu",Frontend.overranges);
    pprintw(w,row++,col,"Last overrange","%s",ftime(tmp,sizeof(tmp),(int64_t)(Frontend.samp_since_over/Frontend.samprate)));
  }
  mvwhline(w,row,0,0,1000);
  mvwaddstr(w,row++,1,"Status");
  pprintw(w,row++,col,"Source","%s",formatsock(&Metadata_source_socket,true));
  pprintw(w,row++,col,"Dest","%s",formatsock(&Frontend.metadata_dest_socket,true));
  pprintw(w,row++,col,"Update interval","%'.2f sec",Refresh_rate);
  pprintw(w,row++,col,"Output status interval","%u",chan->status.output_interval);
  pprintw(w,row++,col,"Status pkts","%'llu",chan->status.packets_out);
  pprintw(w,row++,col,"Control pkts","%'llu",chan->status.packets_in);
  pprintw(w,row++,col,"Blocks since last poll","%'llu",chan->status.blocks_since_poll);
  pprintw(w,row++,col,"Send errors","%'llu",chan->output.errors);
  if(chan->options != 0)
    pprintw(w,row++,col,"Options","0x%llx",(long long)chan->options);
  box(w,0,0);
  mvwaddnstr(w,0,1,Frontend.description,getmaxx(w)-2);
  wnoutrefresh(w);
}

static void display_output(WINDOW *w,struct channel const *chan){
  if(w == NULL)
    return;

  int row = 1;
  int col = 1;
  wmove(w,row,col);
  wclrtobot(w);
  pprintw(w,row++,col,"Source","%s",formatsock(&chan->output.source_socket,true));
  pprintw(w,row++,col,"Dest","%s",formatsock(&chan->output.dest_socket,true));

  pprintw(w,row++,col,"SSRC","%u",chan->output.rtp.ssrc);
  pprintw(w,row++,col,"Timestamp","%'u",chan->output.time_snap);
  pprintw(w,row++,col,"TTL","%d",chan->output.ttl);
  pprintw(w,row++,col,"Payload Type","%u",chan->output.rtp.type);
  pprintw(w,row++,col,"Sample rate","%'d Hz",chan->output.samprate);
  pprintw(w,row++,col,"Encoding","%s",encoding_string(chan->output.encoding));
  pprintw(w,row++,col,"Channels","%d",chan->output.channels);
  pprintw(w,row++,col,"Packets","%'lld",(long long)chan->output.rtp.packets);
  pprintw(w,row++,col,"Packet buffers","%d",chan->output.minpacket);
  if(chan->output.encoding == OPUS || chan->output.encoding == OPUS_VOIP){
    if(chan->opus.bitrate != 0)
      pprintw(w,row++,col,"Opus bitrate","%'d",chan->opus.bitrate);
    else
      pprintw(w,row++,col,"Opus bitrate","auto");
    pprintw(w,row++,col,"Opus dtx","%s",chan->opus.dtx ? "on" : "off");
    char const *appl = opus_application_string(chan->opus.application);
    if(appl != NULL)
      pprintw(w,row++,col,"Opus application","%s",appl);
    pprintw(w,row++,col,"Opus fec","%d%%",chan->opus.fec);
    int bw = opus_bandwidth(NULL,chan->opus.bandwidth);
    pprintw(w,row++,col,"Opus bw","%d kHz",bw/1000);
  }
  box(w,0,0);
  mvwaddstr(w,0,1,"RTP output");
  wnoutrefresh(w);
}

static void display_options(WINDOW *w,struct channel const *chan){
  if(w == NULL)
    return;

  // Demodulator options, can be set with mouse
  int row = 1;
  int col = 1;
  wmove(w,row,col);
  wclrtobot(w);
  switch(chan->demod_type){
  case FM_DEMOD:
    if(!chan->fm.threshold)
      wattron(w,A_UNDERLINE);
    mvwaddstr(w,row++,col,"Th Ext off");
    wattroff(w,A_UNDERLINE);

    if(chan->fm.threshold)
      wattron(w,A_UNDERLINE);
    mvwaddstr(w,row++,col,"Th Ext on");
    wattroff(w,A_UNDERLINE);

    if(!chan->snr_squelch_enable)
      wattron(w,A_UNDERLINE);
    mvwaddstr(w,row++,col,"SNR Sq Off");
    wattroff(w,A_UNDERLINE);
    if(chan->snr_squelch_enable)
      wattron(w,A_UNDERLINE);
    mvwaddstr(w,row++,col,"SNR Sq On");
    wattroff(w,A_UNDERLINE);
    break;
  case WFM_DEMOD:
    // Mono/stereo are only options
    if(chan->output.channels == 1)
      wattron(w,A_UNDERLINE);
    mvwaddstr(w,row++,col,"Mono");
    wattroff(w,A_UNDERLINE);

    if(chan->output.channels == 2)
      wattron(w,A_UNDERLINE);
    mvwaddstr(w,row++,col,"Stereo");
    wattroff(w,A_UNDERLINE);

    if(!chan->snr_squelch_enable)
      wattron(w,A_UNDERLINE);
    mvwaddstr(w,row++,col,"SNR Sq Off");
    wattroff(w,A_UNDERLINE);
    if(chan->snr_squelch_enable)
      wattron(w,A_UNDERLINE);
    mvwaddstr(w,row++,col,"SNR Sq On");
    wattroff(w,A_UNDERLINE);
    break;
  case LINEAR_DEMOD:
    if(chan->linear.env && chan->output.channels == 1)
      wattron(w,A_UNDERLINE);
    mvwaddstr(w,row++,col,"Envelope");
    wattroff(w,A_UNDERLINE);

    if(chan->linear.env && chan->output.channels == 2)
      wattron(w,A_UNDERLINE);
    mvwaddstr(w,row++,col,"Linear+Envelope");
    wattroff(w,A_UNDERLINE);

    if(!chan->linear.env && chan->output.channels == 1)
      wattron(w,A_UNDERLINE);
    mvwaddstr(w,row++,col,"Linear");
    wattroff(w,A_UNDERLINE);

    if(!chan->linear.env && chan->output.channels == 2)
      wattron(w,A_UNDERLINE);
    mvwaddstr(w,row++,col,"I/Q");
    wattroff(w,A_UNDERLINE);

    if(!chan->pll.enable)
      wattron(w,A_UNDERLINE);
    mvwaddstr(w,row++,col,"PLL Off");
    wattroff(w,A_UNDERLINE);

    if(chan->pll.enable && !chan->pll.square)
      wattron(w,A_UNDERLINE);
    mvwaddstr(w,row++,col,"PLL On");
    wattroff(w,A_UNDERLINE);

    if(chan->pll.enable && chan->pll.square)
      wattron(w,A_UNDERLINE);
    mvwaddstr(w,row++,col,"PLL Square");
    wattroff(w,A_UNDERLINE);

    if(!chan->linear.agc)
      wattron(w,A_UNDERLINE);
    mvwaddstr(w,row++,col,"AGC Off");
    wattroff(w,A_UNDERLINE);
    if(chan->linear.agc)
      wattron(w,A_UNDERLINE);
    mvwaddstr(w,row++,col,"AGC On");
    wattroff(w,A_UNDERLINE);

    if(!chan->snr_squelch_enable)
      wattron(w,A_UNDERLINE);
    mvwaddstr(w,row++,col,"SNR Sq Off");
    wattroff(w,A_UNDERLINE);
    if(chan->snr_squelch_enable)
      wattron(w,A_UNDERLINE);
    mvwaddstr(w,row++,col,"SNR Sq On");
    wattroff(w,A_UNDERLINE);

    break;
  default:
    break;
  }
  box(w,0,0);
  mvwaddstr(w,0,1,"Options");
  wnoutrefresh(w);
}

static void display_presets(WINDOW *w,struct channel const *chan){
  if(w == NULL)
    return;

  // Display list of presets defined in presets.conf
  // Can be selected with mouse
  int row = 1;
  int col = 1;
  int npresets = iniparser_getnsec(Pdict);

  for(int i=0;i<npresets;i++){
    char const * const cp = iniparser_getsecname(Pdict,i);
    if(strncasecmp(cp,chan->preset,sizeof(chan->preset)) == 0)
      wattron(w,A_UNDERLINE);
    else
      wattroff(w,A_UNDERLINE);
    mvwaddstr(w,row++,col,cp);
  }
  box(w,0,0);
  mvwaddstr(w,0,1,"Presets");
  wnoutrefresh(w);
}

// Count the actual number of characters in a UTF-8 encoded string
static size_t utf8_strlen(const char *str) {
  size_t length = 0;
  while (*str != '\0') {
    // Check if this byte is the start of a multi-byte character
    if ((*str++ & 0xC0) != 0x80) {
      length++;
    }
  }
  return length;
}


// Like mvwprintw, but right justify the formatted output on the line and overlay with
// a left-justified label
static int pprintw(WINDOW *w,int y, int x, char const *label, char const *fmt,...){
  int maxy;
  int maxx;
  getmaxyx(w,maxy,maxx);

  if(maxy < 0 || maxy > 1000 || maxx < 0 || maxy > 1000){
    return -1;
  }

  // Format the variables
  va_list ap;
  va_start(ap,fmt);
  char result[maxx+1];
  memset(result,0,maxx+1);
  int const r = vsnprintf(result,sizeof(result)-1,fmt,ap);
  va_end(ap);

  if(r == -1)
    return -1;

  // vsnprintf returns character count that *would* be written without limits
  // We want the actual length
  ssize_t const len = utf8_strlen(result);

  ssize_t vstart = maxx - 2 - len; // Allow an extra space for right side of box
  if(vstart < 0)
    vstart = 0; // Too long, truncate on right

  wmove(w,y,x);
  wclrtoeol(w);
  mvwaddstr(w,y,x+(int)vstart,result);
  mvwaddstr(w,y,x,label);
  return 0;
}
// Send empty poll command on specified descriptor
static int send_poll(int ssrc){
  uint8_t cmdbuffer[PKTSIZE];
  uint8_t *bp = cmdbuffer;
  *bp++ = 1; // Command

  uint32_t tag = (uint32_t)random();
  encode_int(&bp,COMMAND_TAG,tag);
  encode_int(&bp,OUTPUT_SSRC,ssrc); // poll specific SSRC, or request ssrc list with ssrc = 0
  encode_eol(&bp);
  size_t const command_len = bp - cmdbuffer;
  if(sendto(Output_fd, cmdbuffer, command_len, 0, &Frontend.metadata_dest_socket,sizeof Frontend.metadata_dest_socket) != (ssize_t)command_len)
    return -1;

  return 0;
}
