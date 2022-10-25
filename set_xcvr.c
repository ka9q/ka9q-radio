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

FILE *Term_stream;
char const *Port;

// flags
int const TX_LOW_POWER = 4;
int const COMPRESSION = 2;
int const BUSY_LOCK = 1;

char const *Config_file;
dictionary *Configtable; // Configtable file descriptor for iniparser

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
  // Always set GPIO pins to output
  gpioSetMode(21,PI_OUTPUT); // PD; 1 = enable, 0 = power down
  gpioSetMode(20,PI_OUTPUT); // PTT: 0 = transmit, 1 = receive
  gpioWrite(21,1); // Enable device

  if(Config_file != NULL){
    // Load and process config file for initial setup
    Configtable = iniparser_load(Config_file);
    if(Configtable == NULL){
      fprintf(stdout,"Can't load config file %s\n",Config_file);
      exit(1);
    }
  
    gpioWrite(20,1); // receive mode (PTT off)
    
    Port = iniparser_getstring(Configtable,"serial","/dev/ttyAMA0");
    int const fd = open(Port,O_RDWR);
    if(fd == -1){
      fprintf(stdout,"Can't open serial port %s: %s\n",Port,strerror(errno));
      exit(1);
    }

    // wideband = 5 kHz deviation, !wideband = 2.5 kHz
    bool const wideband = iniparser_getboolean(Configtable,"wideband",true);
    // Should probably pick a better default
    double const txfreq = iniparser_getdouble(Configtable,"txfreq",146.52);
    double const rxfreq = iniparser_getdouble(Configtable,"rxfreq",146.52);
    // Sleep in microseconds after each write to serial port
    useconds_t const sleep_interval = iniparser_getint(Configtable,"sleep",100000);
    // 0 = no tone
    // 1-38 CTCSS (PL)
    // 39-121 DCS (digital squelch)
    
    int const rxtone = iniparser_getint(Configtable,"rxtone",0);
    int const txtone = iniparser_getint(Configtable,"txtone",0);
    
    int const sq = iniparser_getint(Configtable,"squelch",3); // ?
    int flag = iniparser_getboolean(Configtable,"lowpower",false) ? TX_LOW_POWER : 0;
    flag |= iniparser_getboolean(Configtable,"compression",false) ? COMPRESSION : 0;
    flag |= iniparser_getboolean(Configtable,"busylock",false) ? BUSY_LOCK : 0;
    
    // range 1-8, default 6, higher -> more gain
    int const gain =  iniparser_getint(Configtable,"txgain",6);
    
    int const volume = iniparser_getint(Configtable,"rxgain",1); // ?
    bool const powersave = iniparser_getboolean(Configtable,"powersave",false);
    
    // Vox threshold, 0 = off, 1 = 12 mV, 5 = 7 mV, 8 = 5 mV
    int const vox = iniparser_getint(Configtable,"vox",8);
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
      fprintf(stdout,"tcsetattr succeeded\n");
      tcgetattr(fd,&t);    
      fprintf(stdout,"iflag %x oflag %x cflag %x lflag %x c_cc",
	      t.c_iflag,t.c_oflag,t.c_cflag,t.c_lflag);
      for(int i=0; i < NCCS; i++)
	fprintf(stdout," %x",t.c_cc[i]);
      fputc('\n',stdout);
#endif
    }
    Term_stream = fdopen(fd,"r+");
    if(Term_stream == NULL){
      fprintf(stdout,"Can't fdopen(%d,r+)\n",fd);
      exit(1);
    }
    //    setlinebuf(Term_stream);
    
    // Not really necessary, but the initial \r\n flushes the serial line
    r = fprintf(Term_stream,"\r\n\r\n\r\n");
    usleep(sleep_interval);
    
#if 0
    r = fprintf(Term_stream,"AT+DMOCONNECT\r\n");
    fflush(fp);
    usleep(sleep_interval);
    r = fprintf(Term_stream,"AT+DMOVERQ\r\n");
    usleep(sleep_interval);
#endif
    
    fprintf(stdout,"AT+DMOSETGROUP=%d,%.4lf,%.4lf,%d,%d,%d,%d\n",
		wideband,txfreq,rxfreq,rxtone,txtone,sq,flag);

    r = fprintf(Term_stream,"AT+DMOSETGROUP=%d,%.4lf,%.4lf,%d,%d,%d,%d\r\n",
		wideband,txfreq,rxfreq,rxtone,txtone,sq,flag);
    usleep(sleep_interval);
    
    r = fprintf(Term_stream,"AT+DMOSETMIC=%d,0\r\n",gain); // no "scrambling"
    if(r <= 0)
      fprintf(stdout,"send failed: %s\n",strerror(errno));
    usleep(sleep_interval);
    
    r = fprintf(Term_stream,"AT+DMOAUTOPOWCONTR=%d\r\n",!powersave); // negative logic
    if(r <= 0)
      fprintf(stdout,"send failed: %s\n",strerror(errno));
    usleep(sleep_interval);
    
    r = fprintf(Term_stream,"AT+DMOSETVOLUME=%d\r\n",volume);
    if(r <= 0)
      fprintf(stdout,"send failed: %s\n",strerror(errno));
    usleep(sleep_interval);
    
    r = fprintf(Term_stream,"AT+DMOSETVOX=%d\r\n",vox);
    usleep(sleep_interval);
  }    
  // Key or unkey transmitter?
  if(argc > optind){
    if(strstr(argv[optind],"txon") != NULL){
      gpioWrite(20,0); // transmit mode
    } else if(strstr(argv[optind],"txoff") != NULL){
      gpioWrite(20,1); // receive mode
    } else {
      fprintf(stdout,"Unknown command %s\n",argv[optind]);
    }
  }
  gpioTerminate();

  if(Term_stream != NULL){
    fprintf(stdout,"monitoring %s\n",Port);
    while(1){
      char c = fgetc(Term_stream);
      fputc(c,stdout);
      if(c == '\r')
	fputc('\n',stdout);
      usleep(10000);
    }
  }
  exit(0);
}
