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

int64_t Repeater_tail;
char const *Cwid = "de nocall/r"; // Make this configurable!
double ID_pitch = 800.0;
double ID_level = -29.0;
double ID_speed = 18.0;
char const *Tx_on = "set_xcvr txon";
char const *Tx_off = "set_xcvr txoff";
// IDs must be at least every 10 minutes per FCC 97.119(a)
int64_t Mandatory_ID_interval;
// ID early when carrier is about to drop, to avoid stepping on users
int64_t Quiet_ID_interval;
int Dit_length;
volatile bool PTT_state;
int64_t Last_id_time;

pthread_cond_t PTT_cond = PTHREAD_COND_INITIALIZER;
pthread_mutex_t PTT_mutex = PTHREAD_MUTEX_INITIALIZER;

#if 0
// Send CWID through separate CW daemon (cwd)
// Use non-blocking IO; ignore failures
void send_cwid(void){

  if(Quiet){
    // Debug only, temp
    char result[1024];
    fprintf(stdout,"%s: CW ID started\n",format_gpstime(result,sizeof(result),gps_time_ns()));
  }
  int fd = open("/run/cwd/input",O_NONBLOCK|O_WRONLY);
  if(fd != -1){
    write(fd,Cwid,strlen(Cwid));
    close(fd);
  }
}
#else
// stub version that writes directly to local portaudio output buffer
void send_cwid(void){
  if(Quiet){
    // Debug only, temp
    char result[1024];
    fprintf(stdout,"%s: CW ID started\n",format_gpstime(result,sizeof(result),gps_time_ns()));
  }
  float samples[60 * Dit_length];
  kick_output(); // Start output stream if it was stopped, so we can get current Rptr
  uint32_t wptr = (Rptr + ((long)Playout * DAC_samprate))/1000;
  wptr &= (BUFFERSIZE-1);

  // Don't worry about wrap during write, the mirror will handle it
  for(char const *cp = Cwid; *cp != '\0'; cp++){
    int const samplecount = encode_morse_char(samples,(wchar_t)*cp);
    if(samplecount <= 0)
      break;
    if(Channels == 2){
      for(int i=0;i<samplecount;i++){
	Output_buffer[2*wptr] += samples[i];
	Output_buffer[(2*wptr++ + 1)] += samples[i];
      }
      if(modsub(wptr/2,Wptr,BUFFERSIZE) > 0)
	 Wptr = wptr / 2;
    } else { // Channels == 1
      for(int i=0;i<samplecount;i++)
	Output_buffer[wptr++] += samples[i];
      if(modsub(wptr,Wptr,BUFFERSIZE) > 0)
	 Wptr = wptr;
    }
    kick_output(); // In case it has already drained; the ID could be quite long
    int64_t const sleeptime = BILLION * samplecount / DAC_samprate;
    struct timespec ts;
    ns2ts(&ts,sleeptime);
    nanosleep(&ts,NULL);    // Wait for it to play out
  }
  if(Quiet){
    fprintf(stdout,"CW ID finished\n");
  }
}
#endif



// Repeater control for experimental multi-input repeater
// optional, run only if -t option is given
// Send CW ID at appropriate times
// Drop PTT some time after last write to audio output ring buffer
void *repeater_ctl(void *arg){
  pthread_setname("rptctl");
  (void)arg; // unused

  while(!Terminate){
    // Wait for audio output; set in kick_output()
    pthread_mutex_lock(&PTT_mutex);
    while(!PTT_state)
      pthread_cond_wait(&PTT_cond,&PTT_mutex);
    pthread_mutex_unlock(&PTT_mutex);

    // Turn transmitter on
    if(Tx_on != NULL)
      (void) - system(Tx_on);
    if(Quiet){ // curses display is not on
      // debugging only, temp
      char result[1024];
      fprintf(stdout,"%s: PTT On\n",
	      format_gpstime(result,sizeof(result),LastAudioTime));
    }
    while(true){
      int64_t now = gps_time_ns();
      // When are we required to ID?
      if(now >= Last_id_time + Mandatory_ID_interval){
	// must ID on top of users to satisfy FCC max ID interval
	Last_id_time = now;
	send_cwid();
	now = gps_time_ns(); // send_cwid() has delays
      }
      int64_t const drop_time = LastAudioTime + BILLION * Repeater_tail;
      if(now >= drop_time)
	break;

      // Sleep until possible end of timeout, or next mandatory ID, whichever is first
      int64_t const sleep_time = min(drop_time,Last_id_time + Mandatory_ID_interval) - now;
      if(sleep_time > 0){
	struct timespec ts;
	ns2ts(&ts,sleep_time);
	nanosleep(&ts,NULL);
      }
    }
    // time to drop transmitter
    // See if we can ID early before dropping, to avoid a mandatory ID on the next transmission
    int64_t now = gps_time_ns();
    if(now > Last_id_time + Mandatory_ID_interval / 2){
      Last_id_time = now;
      send_cwid();
      now = gps_time_ns();
    }
    pthread_mutex_lock(&PTT_mutex);
    PTT_state = false;
    pthread_mutex_unlock(&PTT_mutex);
    Last_xmit_time = gps_time_ns();
    if(Quiet){
      // debug only, temp
      char result[1024];
      fprintf(stdout,"%s: PTT Off\n",format_gpstime(result,sizeof(result),gps_time_ns()));
    }
    if(Tx_off != NULL)
      (void) - system(Tx_off);
  }
  return NULL;
}

