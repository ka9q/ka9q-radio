// $Id: show-sig.c,v 1.7 2022/08/05 06:35:10 karn Exp $
// Display signal levels
// Copyright 2021 Phil Karn, KA9Q
// Adapted from show-pkt.c

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
// Fix this name conficts with status.h
int decode_rtp_status(unsigned char const *buffer,int length);
int decode_frontend_status(unsigned char const *buffer,int length);


const char *App_path;
int Verbose,Dump;

struct sockaddr_storage Output_metadata_dest_address;
struct sockaddr_storage Output_metadata_source_address;
struct sockaddr_storage Input_metadata_dest_address; // needed to listen to front end directly

char const *Input_metadata_source_socket = "";
char const *Input_metadata_dest_socket = "";
char Description[256];

char const *Output_data_source_socket = "";
char const *Output_data_dest_socket = "";
float Output_level;
int LNA_gain;
int Mixer_gain;
int IF_gain;
int Input_SSRC;
int Output_SSRC;
char const *Output_metadata_source_socket = "";
char const *Output_metadata_dest_socket = "";
double Frequency;
float Low_edge,High_edge;
float IF_power;
float Baseband_power;
float Noise_density;
float Demod_snr = NAN;
float Headroom;
float Gain;
float Output_level;

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
int FE_fd = -1;

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
    struct timespec const timeout = { 0, 100000000 }; // 100 ms -> 10 Hz
    fd_set fdset;
    FD_ZERO(&fdset);
    FD_SET(Radio_fd,&fdset);
    if(FE_fd != -1)
      FD_SET(FE_fd,&fdset);      
    int n = max(Radio_fd,FE_fd) + 1;
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
    if(FE_fd == -1 && ((struct sockaddr_in *)&Input_metadata_dest_address) -> sin_family != 0){
      FE_fd = setup_mcast_in(NULL,(struct sockaddr *)&Input_metadata_dest_address,0);
    }
    if(FD_ISSET(FE_fd,&fdset)){
      // Message from the front end
      unsigned char buffer[8192];
      memset(buffer,0,sizeof(buffer));
      socklen_t ssize = sizeof(Output_metadata_source_address);
      int length = recvfrom(FE_fd,buffer,sizeof(buffer),0,(struct sockaddr *)&Output_metadata_source_address,&ssize);
      if(length <= 0){
	usleep(100000); // don't burn time in a tight error loop
	continue;
      }
      // Parse entries - careful, E.g,, radio's input is front end's output
      if(length >= 2 && buffer[0] == 0){ // Ignore our own command packets
	decode_frontend_status(buffer+1,length-1);
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

  int data_indent = 10;
  int header_indent = 5;

  werase(stdscr);
  hline(0,31);

  mvprintw(row++,header_indent,"Front end metadata"); // on top of line

  mvprintw(row++,col,"%s -> %s\n",Input_metadata_source_socket,Input_metadata_dest_socket);
  mvprintw(row++,col,"SSRC %x\n",Input_SSRC);
  mvprintw(row++,col,"%s\n",Description);
  hline(0,31);  
  mvprintw(row++,header_indent,"Radio metadata"); // on top of line

  mvprintw(row++,col,"%s -> %s\n",Output_metadata_source_socket,Output_metadata_dest_socket);
  mvprintw(row++,col,"SSRC %x\n",Output_SSRC);
  mvprintw(row++,col,"Frequency %'.3lf Hz\n",Frequency);
  hline(0,31);
  mvprintw(row++,header_indent,"Signal levels"); // overwrite line
  mvprintw(row++,col,"A/D Power      %*.1f dBFS\n",data_indent,Output_level);
  // These should be floats
  mvprintw(row++,col,"LNA Gain     %*d   dB\n",data_indent,LNA_gain);
  mvprintw(row++,col,"Mixer Gain   %*d   dB\n",data_indent,Mixer_gain);
  mvprintw(row++,col,"IF Gain      %*d   dB\n",data_indent,IF_gain);
  mvprintw(row++,col,"RF/IF Power    %*.1f dB\n",data_indent,IF_power);
  mvprintw(row++,col,"Baseband Power %*.1f dB\n",data_indent,Baseband_power);
  mvprintw(row++,col,"Noise density  %*.1f dB/Hz\n",data_indent,Noise_density);
  float bw = 10*log10(fabsf(High_edge - Low_edge));
  float noise_power = dB2power(Noise_density + bw);
  // S = total baseband power - noise power (bw*N0) in linear units
  float sn0 = power2dB(dB2power(Baseband_power) - noise_power ) - Noise_density;

  float snr = sn0 - bw;
  mvprintw(row++,col,"S/N0           %*.1f dB Hz\n",data_indent,sn0);
  mvprintw(row++,col,"Bandwidth      %*.1f dB Hz\n",data_indent,bw);
  mvprintw(row++,col,"SNR            %*.1f dB\n",data_indent,snr);
  if(!isnan(Demod_snr))
    mvprintw(row++,col,"Demod SNR      %*.1f dB\n",data_indent,Demod_snr);

  mvprintw(row++,col,"Gain           %*.1f dB\n",data_indent,Gain);
  mvprintw(row++,col,"Output level   %*.1f dB\n",data_indent,Output_level);
  mvprintw(row++,col,"Headroom       %*.1f dB\n",data_indent,Headroom);

  wnoutrefresh(stdscr);
  doupdate(); 

}




// Decode incoming status message from the front end
int decode_frontend_status(unsigned char const *buffer,int length){
  unsigned char const *cp = buffer;
  while(cp - buffer < length){
    enum status_type type = *cp++; // increment cp to length field

    if(type == EOL)
      break; // end of list
    
    unsigned int optlen = *cp++;
    if(cp - buffer + optlen >= length)
      break; // invalid length; we can't continue to scan

    switch(type){
    case OUTPUT_LEVEL:
      Output_level = decode_float(cp,optlen);
      break;
    case LNA_GAIN:
      LNA_gain = decode_int(cp,optlen);
      break;
    case MIXER_GAIN:
      Mixer_gain = decode_int(cp,optlen);
      break;
    case IF_GAIN:
      IF_gain = decode_int(cp,optlen);
      break;

    default:
      ;
    }
    cp += optlen;
  }

  return 0;
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
    case DESCRIPTION:
      decode_string(cp,optlen,Description,sizeof(Description));
      break;

    case INPUT_METADATA_SOURCE_SOCKET:
      {
	struct sockaddr_storage tmp;
	Input_metadata_source_socket = formatsock(decode_socket(&tmp,cp,optlen));
      }	
      break;
    case INPUT_METADATA_DEST_SOCKET:
      {
	// this is the metadata from the front end. We'll create a socket of our own to monitor it
	Input_metadata_dest_socket =formatsock(decode_socket(&Input_metadata_dest_address,cp,optlen));
      }	
      break;
    case INPUT_SSRC:
      Input_SSRC = decode_int(cp,optlen);
      break;
    case OUTPUT_SSRC:
      Output_SSRC = decode_int(cp,optlen);
      break;
    case IF_POWER:
      IF_power = decode_float(cp,optlen);
      break;
    case RADIO_FREQUENCY:
      Frequency = decode_double(cp,optlen);
      break;
    case LOW_EDGE:
      Low_edge = decode_float(cp,optlen);
      break;
    case HIGH_EDGE:
      High_edge = decode_float(cp,optlen);
      break;
    case BASEBAND_POWER:
      Baseband_power = decode_float(cp,optlen);
      break;
    case NOISE_DENSITY:
      Noise_density = decode_float(cp,optlen);
      break;
    case DEMOD_SNR:
      Demod_snr = decode_float(cp,optlen);
      break;
    case HEADROOM:
      Headroom = decode_float(cp,optlen);
      break;
    case GAIN:
      Gain = decode_float(cp,optlen);
      break;
    case OUTPUT_LEVEL:
      Output_level = decode_float(cp,optlen);      
      break;
    default:
      ;
    }
    cp += optlen;
  }

  return 0;
}


