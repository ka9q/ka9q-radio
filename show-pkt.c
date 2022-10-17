// $Id: show-pkt.c,v 1.7 2022/08/05 06:35:10 karn Exp $
// Display RTP statistics
// Copyright 2021 Phil Karn, KA9Q
// Adapted from control.c

#define _GNU_SOURCE 1
#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#if defined(linux)
#include <bsd/string.h>
#endif

#include <locale.h>
#include <ncurses.h>

#include "misc.h"
#include "multicast.h"
#include "status.h"

char Locale[256] = "en_US.UTF-8";
int decode_rtp_status(unsigned char const *buffer,int length);


const char *App_path;
int Verbose,Dump;

struct sockaddr_storage Output_metadata_dest_address;
struct sockaddr_storage Output_metadata_source_address;
int Cmd_cnt;
char const *Input_data_source_socket = "";
char const *Input_data_dest_socket = "";
char const *Input_metadata_source_socket = "";
char const *Input_metadata_dest_socket = "";
int Input_SSRC;
int Input_metadata_packets;
int Input_data_packets;
int Input_drops;
int Input_dupes;
char const *Output_data_source_socket = "";
char const *Output_data_dest_socket = "";
int Output_SSRC;
int Output_TTL;
int Output_metadata_packets;
int Output_data_packets;
char const *Output_metadata_source_socket = "";
char const *Output_metadata_dest_socket = "";

void doscreen(void);

static FILE *Tty;
static SCREEN *Term;

void display_cleanup(void){
  echo();
  nocbreak();
  endwin();
  if(Term)
    delscreen(Term);
  Term = NULL;
  if(Tty)
    fclose(Tty);
  Tty = NULL;
}

int Radio_fd;

// Thread to display receiver state, updated at 10Hz by default
// Uses the ancient ncurses text windowing library
// Also services keyboard, mouse and tuning knob, if present
// I had been running this at normal priority, but it can start new demodulators
// so it must also run at preferred priority
int main(int argc,char *argv[]){
  App_path = argv[0];
  {
    int c;

    while((c = getopt(argc,argv,"vd")) != EOF){
      switch(c){
      case 'v':
	Verbose++;
	break;
      }
    }
  }

  {
    // We assume en_US.UTF-8, or anything with a thousands grouping character
    char const * const cp = getenv("LANG");
    if(cp != NULL){
      strlcpy(Locale,cp,sizeof(Locale));
    }
  }

  setlocale(LC_ALL,Locale); // Set either the hardwired default or the value of $LANG if it exists
  atexit(display_cleanup);

  Radio_fd = setup_mcast_in(argv[optind],(struct sockaddr *)&Output_metadata_dest_address,2);
  if(Radio_fd == -1){
    fprintf(stderr,"Can't listen to %s\n",argv[optind]);
    exit(1);
  }
  /* Main loop:
     Read radio/front end status from network with 100 ms timeout to serve as polling rate
     Update local status; retransmit radio command if necessary
     Repaint display windows
     Poll keyboard and process user commands
  */
  
  // Set up display subwindows
  // ncurses setup
  atexit(display_cleanup);
#if 0
  Tty = fopen("/dev/tty","r+");
  Term = newterm(NULL,Tty,Tty);
  set_term(Term);
#endif
  initscr();
  keypad(stdscr,TRUE);
  meta(stdscr,TRUE);
  timeout(0); // Don't block in getch()
  cbreak();
  noecho();


  for(;;){
    struct timespec const timeout = {0, 100000000}; // 100 ms -> 10 Hz

    fd_set fdset;
    FD_ZERO(&fdset);
    FD_SET(Radio_fd,&fdset);
    int n = Radio_fd + 1;
    n = pselect(n,&fdset,NULL,NULL,&timeout,NULL);

    if(FD_ISSET(Radio_fd,&fdset)){
      // Message from the radio program
      unsigned char buffer[8192];
      memset(buffer,0,sizeof(buffer));
      socklen_t ssize = sizeof(Output_metadata_source_address);
      int length = recvfrom(Radio_fd,buffer,sizeof(buffer),0,(struct sockaddr *)&Output_metadata_source_address,&ssize);
      if(length <= 0){
	usleep(100000); // don't burn time in a tight error loop
	continue;
      }
      Output_metadata_source_socket = formatsock(&Output_metadata_source_address); 
      Output_metadata_dest_socket = formatsock(&Output_metadata_dest_address);

      // Parse entries
      if(length >= 2 && buffer[0] == 0){ // Ignore our own command packets
	decode_rtp_status(buffer+1,length-1);

      }
    }
    doscreen();
  }
  endwin();
  set_term(NULL);
  if(Term != NULL)
    delscreen(Term);
  //  if(Tty != NULL)
  //    fclose(Tty);
  
  exit(0);
}


void doscreen(void){
  int row = 0;
  int col = 0;

  int data_indent = 30;
  int header_indent = 5;

  werase(stdscr);
  hline(0,20);
  mvprintw(row++,header_indent,"Input data"); // on top of line

  mvprintw(row++,col,"%s -> %s\n",Input_data_source_socket,Input_data_dest_socket);
  mvprintw(row,col,"%*x\n",data_indent,Input_SSRC);
  mvprintw(row++,col,"Input SSRC");
  mvprintw(row,col,"%'*d\n",data_indent,Input_data_packets);
  mvprintw(row++,col,"Input data pkts");

  mvprintw(row,col,"%'*d\n",data_indent,Input_drops);
  mvprintw(row++,col,"Input data drops");

  mvprintw(row,col,"%'*d\n",data_indent,Input_dupes);
  mvprintw(row++,col,"Input data dups");

  row++;
  move(row,0);
  hline(0,20);
  mvprintw(row++,header_indent,"Input meta"); // on top of line
  mvprintw(row++,col,"%s -> %s\n",Input_metadata_source_socket,Input_metadata_dest_socket);
  mvprintw(row,col,"%'*d\n",data_indent,Input_metadata_packets);
  mvprintw(row++,col,"Input meta pkts");

  row++;
  move(row,0);
  hline(0,20);
  mvprintw(row++,header_indent,"Output data"); // On top of ine
  mvprintw(row++,col,"%s -> %s\n",Output_data_source_socket,Output_data_dest_socket);
  mvprintw(row,col,"%*x\n",data_indent,Output_SSRC);
  mvprintw(row++,col,"Output SSRC");

  mvprintw(row,col,"%*d\n",data_indent,Output_TTL);
  mvprintw(row++,col,"Output TTL");

  mvprintw(row,col,"%'*d",data_indent,Output_data_packets);
  mvprintw(row++,col,"Output data pkts");

  row++;
  move(row,0);
  hline(0,20);
  mvprintw(row++,header_indent,"Output meta");
  mvprintw(row++,col,"%s -> %s\n",Output_metadata_source_socket,Output_metadata_dest_socket);

  mvprintw(row,col,"%'*d",data_indent,Output_metadata_packets);
  mvprintw(row++,col,"Output meta pkts");
  mvprintw(row,col,"%'*d\n",data_indent,Cmd_cnt);
  mvprintw(row++,col,"Commands");


  wnoutrefresh(stdscr);
  doupdate(); 

}




// Decode incoming status message from the radio program
int decode_rtp_status(unsigned char const *buffer,int length){
  unsigned char const *cp = buffer;
  while(cp - buffer < length){
    enum status_type type = *cp++; // increment cp to length field

    if(type == EOL)
      break; // end of list
    
    unsigned int optlen = *cp++;
    if(cp - buffer + optlen >= length)
      break; // invalid length; we can't continue to scan

    switch(type){
    case CMD_CNT:
      Cmd_cnt = decode_int(cp,optlen);
      break;
    case INPUT_DATA_SOURCE_SOCKET:
      {
	struct sockaddr_storage tmp;
	Input_data_source_socket = formatsock(decode_socket(&tmp,cp,optlen));
      }	
      break;
    case INPUT_DATA_DEST_SOCKET:
      {
	struct sockaddr_storage tmp;
	Input_data_dest_socket = formatsock(decode_socket(&tmp,cp,optlen));
      }	
      break;
    case INPUT_METADATA_SOURCE_SOCKET:
      {
	struct sockaddr_storage tmp;
	Input_metadata_source_socket = formatsock(decode_socket(&tmp,cp,optlen));
      }	
      break;
    case INPUT_METADATA_DEST_SOCKET:
      {
	struct sockaddr_storage tmp;
	Input_metadata_dest_socket =formatsock(decode_socket(&tmp,cp,optlen));
      }	
      break;
    case INPUT_SSRC:
      Input_SSRC = decode_int(cp,optlen);
      break;
    case INPUT_METADATA_PACKETS:
      Input_metadata_packets = decode_int(cp,optlen);
      break;
    case INPUT_DATA_PACKETS:
      Input_data_packets = decode_int(cp,optlen);
      break;
    case INPUT_DROPS:
      Input_drops = decode_int(cp,optlen);
      break;
    case INPUT_DUPES:
      Input_dupes = decode_int(cp,optlen);
      break;
    case OUTPUT_DATA_SOURCE_SOCKET:
      {
	struct sockaddr_storage tmp;
	Output_data_source_socket = formatsock(decode_socket(&tmp,cp,optlen));
      }	
      break;
    case OUTPUT_DATA_DEST_SOCKET:
      {
	struct sockaddr_storage tmp;
	Output_data_dest_socket = formatsock(decode_socket(&tmp,cp,optlen));
      }	
      break;
    case OUTPUT_SSRC:
      Output_SSRC = decode_int(cp,optlen);
      break;
    case OUTPUT_TTL:
      Output_TTL = decode_int(cp,optlen);
      break;
    case OUTPUT_METADATA_PACKETS:
      Output_metadata_packets = decode_int(cp,optlen);
      break;
    case OUTPUT_DATA_PACKETS:
      Output_data_packets = decode_int(cp,optlen);
      break;
    default:
      break;
    }
    // Implicitly
    cp += optlen;
  }

  return 0;
}


