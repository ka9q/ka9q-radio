// Control transmitter on FM transceiver daughter board on Raspberry Pi
// Experimental FM repeater option in monitor.c
// Copyright 2022-2023 Phil Karn, KA9Q
#include <termios.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <pigpio.h>
#include <iniparser/iniparser.h>
#include <stdarg.h>
#include <sysexits.h>

#include "config.h"

FILE *Term_stream;
char const *Port;
useconds_t Sleep_interval;

// flags
int const TX_LOW_POWER = 4;
int const COMPRESSION = 2;
int const BUSY_LOCK = 1;

char const *Config_file;
char const *Section = "radio";
dictionary *Configtable; // Configtable file descriptor for iniparser

int sendcmd(const char *fmt, ...){
  char cmdline[2048];

  va_list ap;
  va_start(ap,fmt);
  int n = vsnprintf(cmdline,sizeof(cmdline),fmt,ap);
  va_end(ap);
  fputs(cmdline,stderr);
  fputs(cmdline,Term_stream);
  fflush(Term_stream);
  usleep(Sleep_interval);
  return n;
}


int main(int argc,char *argv[]){

  int c;
  while((c = getopt(argc,argv,"f:")) != -1){
    switch(c){
    case 'f':
      Config_file = optarg;
      break;
    default:
      break;
    }
  }
  gpioInitialise();
  gpioSetMode(18,PI_ALT5); // Mono audio on GPIO18, physical pin 12
  gpioSetPullUpDown(18, PI_PUD_OFF);
  // Always set GPIO pins to output
  gpioSetMode(21,PI_OUTPUT); // PD; 1 = enable, 0 = power down
  gpioSetPullUpDown(21, PI_PUD_OFF);
  gpioSetMode(20,PI_OUTPUT); // PTT: 0 = transmit, 1 = receive
  gpioSetPullUpDown(20, PI_PUD_OFF);
  gpioWrite(21,1); // Enable device
  // Key or unkey transmitter?
  if(argc > optind){
    if(strstr(argv[optind],"txon") != NULL || strstr(argv[optind],"on") != NULL){
      gpioWrite(20,0); // transmit mode
    } else if(strstr(argv[optind],"txoff") != NULL || strstr(argv[optind],"off") != NULL){
      gpioWrite(20,1); // receive mode
    } else {
      fprintf(stderr,"Unknown command %s\n",argv[optind]);
    }
    exit(EX_OK);
  }

  gpioWrite(20,1); // receive mode (PTT off)

  if(Config_file != NULL){
    // Load and process config file for initial setup
    Configtable = iniparser_load(Config_file);
    if(Configtable == NULL){
     fprintf(stderr,"Can't load config file %s\n",Config_file);
      exit(EX_USAGE);
    }

    // route UART pins (already done?)
    gpioSetMode(14,PI_ALT0);
    gpioSetMode(15,PI_ALT0);

    Port = strdup(config_getstring(Configtable,Section,"serial","/dev/ttyAMA0")); // iniparser storage is dynamic
    int fd = open(Port,O_RDWR);
    if(fd == -1){
      fprintf(stderr,"Can't open serial port %s: %s\n",Port,strerror(errno));
      exit(EX_NOINPUT);
    }

    // wideband = 5 kHz deviation, !wideband = 2.5 kHz
    bool const wideband = config_getboolean(Configtable,Section,"wideband",true);

    // Should probably pick a better default
    double const txfreq = config_getdouble(Configtable,Section,"txfreq",146.52);
    double const rxfreq = config_getdouble(Configtable,Section,"rxfreq",146.52);
    // Sleep in microseconds after each write to serial port
    Sleep_interval = config_getint(Configtable,Section,"sleep",100000);

    // CTCSS tone indices
    // Note this list skips some tones that the Baofeng radios support
    // even though they use the same transceiver chip (1846).
    // 0 = no tone
    // 1 = 67.0
    // skipped: 69.3
    // 2 = 71.9
    // 3 = 74.4
    // 4 = 77.0
    // 5 = 79.7
    // 6 = 82.5
    // 7 = 85.4
    // 8 = 88.5
    // 9 = 91.5
    // 10 = 94.8
    // 11 = 97.4
    // 12 = 100.0
    // 13 = 103.5
    // 14 = 107.2
    // 15 = 110.9
    // 16 = 114.8
    // 17 = 118.8
    // 18 = 123.0
    // 19 = 127.3
    // 20 = 131.8
    // 21 = 136.5
    // 22 = 141.3
    // 23 - 146.2
    // skipped: 150.0 (NATO)
    // 24 = 151.4
    // 25 = 156.7
    // skipped: 159.8
    // 26 = 162.2
    // skipped: 165.5
    // 27 = 167.9
    // skipped: 171.3
    // 28 = 173.8
    // skipped: 177.3
    // 29 = 179.9
    // skipped: 183.5
    // 30 = 186.2
    // skipped: 189.9
    // 31 = 192.8
    // skipped: 196.6, 199.5
    // 32 = 203.5
    // skipped: 206.5
    // 33 = 210.7
    // skipped: 213.8
    // 34 = 218.1
    // skipped: 221.3
    // 35 = 225.7
    // skipped: 229.1
    // 36 = 233.6
    // skipped: 237.1
    // 37 = 241.8
    // skipped: 245.5
    // 38 = 250.3
    // skipped: 254.1
    // 39-121 DCS (digital squelch)
    
    int const rxtone = config_getint(Configtable,Section,"rxtone",0);
    int const txtone = config_getint(Configtable,Section,"txtone",0);
    
    int const sq = config_getint(Configtable,Section,"squelch",3); // ?
    int flag = config_getboolean(Configtable,Section,"lowpower",false) ? TX_LOW_POWER : 0;
    flag |= config_getboolean(Configtable,Section,"compression",false) ? COMPRESSION : 0;
    flag |= config_getboolean(Configtable,Section,"busylock",false) ? BUSY_LOCK : 0;
    
    // range 1-8, default 6, higher -> more gain
    int const gain =  config_getint(Configtable,Section,"txgain",6);
    
    int const volume = config_getint(Configtable,Section,"rxgain",1); // ?
    bool const powersave = config_getboolean(Configtable,Section,"powersave",false);
    
    // Vox threshold, 0 = off, 1 = 12 mV, 5 = 7 mV, 8 = 5 mV
    int const vox = config_getint(Configtable,Section,"vox",8);
    int const scramble = config_getint(Configtable,Section,"scramble",0);

    iniparser_freedict(Configtable); // Done with config file
    
    usleep(1000000); // transceiver should power up in 500 ms
    
    struct termios t;
    tcgetattr(fd,&t);
    cfmakeraw(&t);
    cfsetspeed(&t,B9600);
    int r = tcsetattr(fd,TCSANOW,&t);
    if(r != 0){
      perror("tcsetattr");
    } else {
#if DEBUG
      fprintf(stderr,"tcsetattr succeeded\n");
      tcgetattr(fd,&t);    
      fprintf(stderr,"iflag %x oflag %x cflag %x lflag %x c_cc",
	      t.c_iflag,t.c_oflag,t.c_cflag,t.c_lflag);
      for(int i=0; i < NCCS; i++)
	fprintf(stderr," %x",t.c_cc[i]);
      fputc('\n',stderr);
#endif
    }

    Term_stream = fdopen(fd,"r+");
    if(Term_stream == NULL){
      fprintf(stderr,"Can't fdopen(%d,r+)\n",fd);
      exit(EX_IOERR);
    }
    // Not really necessary, but the initial \r\n flushes the serial line
    // NB: All commands must end with \r\n, not just \n!
    sendcmd("\r\n");
    
#if DEBUG
    sendcmd("AT+DMOCONNECT\r\n");
    sendcmd("AT+DMOVERQ\r\n");
#endif
    sendcmd("AT+DMOSETGROUP=%d,%.4lf,%.4lf,%d,%d,%d,%d\r\n",
	    wideband,txfreq,rxfreq,rxtone,sq,txtone,flag);
    sendcmd("AT+DMOSETMIC=%d,%d\r\n",gain,scramble); // no "scrambling"
    sendcmd("AT+DMOAUTOPOWCONTR=%d\r\n",!powersave); // negative logic
    sendcmd("AT+DMOSETVOLUME=%d\r\n",volume);
    sendcmd("AT+DMOSETVOX=%d\r\n",vox);
#if DEBUG
    bool cr_seen = false;
    while(true){
      char const c = fgetc(Term_stream);
      if(cr_seen && c != '\n')	 
	fputc('\n',stderr); // ensure new line after cr
      fputc(c,stderr);
      cr_seen = (c == '\r'); // Assign comparison
      usleep(10000);
    }
    fclose(Term_stream); Term_stream = NULL;
    close(fd); fd = -1;
#endif  
  }
  gpioTerminate();
  exit(EX_OK);
}
