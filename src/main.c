// Core of KA9Q radiod
// downconvert, filter, demodulate, multicast output
// Copyright 2017-2025, Phil Karn, KA9Q, karn@ka9q.net
#define _GNU_SOURCE 1
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <string.h>
#if defined(linux)
#include <bsd/string.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <locale.h>
#include <signal.h>
#include <getopt.h>
#include <sysexits.h>
#include <strings.h>
#include <fenv.h>

#include "radio.h"

// Command line and environ params
char const *Config_file;
const char *App_path;
int Verbose;
static char const *Locale = "en_US.UTF-8";
volatile bool Stop_transfers = false; // Request to stop data transfers; how should this get set?
char const *Name; // List of valid config keys in [global] section, for error checking

static void closedown(int);
static void verbosity(int);

#ifndef NDEBUG
static void fpe_handler(int sig){
  (void)sig;
  fprintf(stderr,"SIGFPE: floating point exception\n");
  abort();
}
#endif


// The main program sets up the demodulator parameter defaults,
// overwrites them with command-line arguments and/or state file settings,
// initializes the various local oscillators, pthread mutexes and conditions
// sets up multicast I/Q input and PCM audio output
// Sets up the input half of the pre-detection filter
// starts the RTP input and downconverter/filter threads
// sets the initial demodulation mode, which starts the demodulator thread
// catches signals and eventually becomes the user interface/display loop
int main(int argc,char *argv[]){
  App_path = argv[0];

  VERSION();
#ifndef NDEBUG
  fprintf(stderr,"Assertion checking enabled, execution will be slower\n");
  fprintf(stderr,"Floating point exception traps enabled\n");
  feclearexcept(FE_ALL_EXCEPT);
  feenableexcept(FE_INVALID|FE_DIVBYZERO);
  signal(SIGFPE,fpe_handler);
#endif

  setlinebuf(stderr);

  struct timespec start_realtime;
  clock_gettime(CLOCK_MONOTONIC,&start_realtime);

  // Set up program defaults
  // Some can be overridden by command line args
  {
    // The display thread assumes en_US.UTF-8, or anything with a thousands grouping character
    // Otherwise the cursor movements will be wrong
    char const * const cp = getenv("LANG");
    if(cp != NULL)
      Locale = cp;
  }
  setlocale(LC_ALL,Locale); // Set either the hardwired default or the value of $LANG if it exists

  int c;
  while((c = getopt(argc,argv,"N:vV")) != -1){
    switch(c){
    case 'V': // Already shown above
      exit(EX_OK);
    case 'v':
      Verbose++;
      break;
    case 'N':
      Name = optarg;
      break;
    default: // including 'h'
      fprintf(stderr,"Unknown command line option %c\n",c);
      fprintf(stderr,"Usage: %s [-I] [-N name] [-h] [-p fftw_plan_time_limit] [-v [-v] ...] <CONFIG_FILE>\n", argv[0]);
      exit(EX_USAGE);
    }
  }

  // Graceful signal catch
  signal(SIGINT,closedown);
  signal(SIGQUIT,closedown);
  signal(SIGTERM,closedown);
  signal(SIGPIPE,SIG_IGN);
  signal(SIGUSR1,verbosity);
  signal(SIGUSR2,verbosity);

  if(argc <= optind){
    fprintf(stderr,"Configtable file missing\n");
    exit(EX_NOINPUT);
  }
  Config_file = argv[optind];
  if(Name == NULL){
    // Extract name from config file pathname
    Name = argv[optind]; // Ah, just use whole thing
  }

  int const n = loadconfig(Config_file);
  if(n < 0){
    fprintf(stderr,"Can't load config file %s\n",Config_file);
    exit(EX_NOINPUT);
  }
  fprintf(stderr,"%d total demodulators started\n",n);
  // Measure CPU usage
  int sleep_period = 60;
  struct timespec last_realtime = start_realtime;
  struct timespec last_cputime = {0};
  while(true){
    sleep(sleep_period);
    if(Verbose){
      struct timespec new_realtime;
      clock_gettime(CLOCK_MONOTONIC,&new_realtime);
      double total_real = new_realtime.tv_sec - start_realtime.tv_sec
	+ 1e-9 * (new_realtime.tv_nsec - start_realtime.tv_nsec);

      struct timespec new_cputime;
      clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&new_cputime);
      double total_cpu = new_cputime.tv_sec + 1e-9 * (new_cputime.tv_nsec);

      double total_percent = 100. * total_cpu / total_real;

      double period_real = new_realtime.tv_sec - last_realtime.tv_sec
	+ 1e-9 * (new_realtime.tv_nsec - last_realtime.tv_nsec);

      double period_cpu =  new_cputime.tv_sec - last_cputime.tv_sec
	+ 1e-9 * (new_cputime.tv_nsec - last_cputime.tv_nsec);

      double period_percent = 100. * period_cpu / period_real;

      last_realtime = new_realtime;
      last_cputime = new_cputime;
      fprintf(stderr,"CPU usage: %.1lf%% since start, %.1lf%% in last %.1lf sec\n",
	      total_percent, period_percent,period_real);
    }
  }
  exit(EX_OK); // Can't happen
}
static void closedown(int a){
  char message[] = "Received signal, shutting down\n";

  ssize_t r = write(1,message,strlen(message));
  (void)r; // shut up compiler
  Stop_transfers = true;
  sleep(1); // pause for threads to see it
  _exit(a == SIGTERM ? EX_OK : EX_SOFTWARE); // Success when terminated by systemd
}

// Increase or decrease logging level (thanks AI6VN for idea)
static void verbosity(int a){
  if(a == SIGUSR1)
    Verbose++;
  else if(a == SIGUSR2)
    Verbose = (Verbose <= 0) ? 0 : Verbose - 1;
  else
    return;
}
