// Read and record PCM audio streams
// Copyright 2021 Phil Karn, KA9Q
//
// Modified "wspr-decoded" to "wd-record" to record 1 minute .wav files, synchronized to the UTC
// second by Clint Turner, KA7OEI for use with the "WSPRDaemon" code by Rob Robinett, AI6VN.
//
// July 14, 2023 Rob Robinett, AI6VN.  Modified Clint's code to fully support creating the 1 minute long .wav files needed by WD
//
// TO DO:
//  - Cleanup from previous "wspr-decoded" version (e.g. remove unneeded variables/code)
//

char const * wd_record_version = "This is wd-record version 0.3 which recovers from restarts of radiod";

#define _GNU_SOURCE 1
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#if defined(linux)
#include <bsd/string.h>
#endif
#include <math.h>
#include <complex.h>
#undef I
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <fcntl.h>
#include <locale.h>
#include <signal.h>
#include <netdb.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sysexits.h>
#include "misc.h"
#include "attr.h"
#include "multicast.h"

// size of stdio buffer for disk I/O
// This should be large to minimize write calls, but how big?
#define BUFFERSIZE (1<<16)

// Simplified .wav file header
// http://soundfile.sapp.org/doc/WaveFormat/
struct wav {
  char ChunkID[4];
  int32_t ChunkSize;
  char Format[4];

  char Subchunk1ID[4];
  int32_t Subchunk1Size;
  int16_t AudioFormat;
  int16_t NumChannels;
  int32_t SampleRate;
  int32_t ByteRate;
  int16_t BlockAlign;
  int16_t BitsPerSample;

  char SubChunk2ID[4];
  int32_t Subchunk2Size;
};

// One for each session being recorded
struct session {
  struct session *prev;
  struct session *next;
  struct sockaddr sender;   // Sender's IP address and source port

  char filename[PATH_MAX];
  struct wav header;

  uint32_t ssrc;               // RTP stream source ID
  struct rtp_state rtp_state;
  
  int type;                    // RTP payload type (with marker stripped)
  int channels;                // 1 (PCM_MONO) or 2 (PCM_STEREO)
  unsigned int samprate;

  FILE *fp;                    // File being recorded
  void *iobuffer;              // Big buffer to reduce write rate

  int64_t SamplesWritten;
  int64_t TotalFileSamples;
  uint32_t first_sample_number;
};

int          Searching_for_first_minute = 1;    // 1 => don't write to wav file until transition from second 59 to second zero.  
                                           // This should be done for each source stream, but wd-record only records one stream so this can be a global variable
int          Samples_per_second = 0;            // If non-zero from -S RATE arg, overrides the sp->samprate value which is infered from sample size
uint32_t     sample_number_of_first_sample_in_current_wav_file = 0;   // The timestamp in the radiod RTP files is actually the sample number 

char const *App_path;
int verbosity;
int Keep_wav;
int csec, osec, trig;
char PCM_mcast_address_text[256];
char const *Recordings = ".";
char const *Wsprd_command = "wsprd -a %s/%u -o 2 -f %.6lf -w -d %s";

struct sockaddr Sender;
struct sockaddr Input_mcast_sockaddr;
int Input_fd;
struct session *Sessions;

void closedown(int a);
void input_loop(void);
void cleanup(void);
struct session *create_session( struct rtp_header *,  const int wav_start_epoch, const int tuning_freq_hz );
void close_session(struct session **p);
void flush_session(struct session **p);
uint32_t Ssrc=0; // Requested SSRC

int main(int argc,char *argv[]){
  App_path = argv[0];
  char const * locale = getenv("LANG");
  setlocale(LC_ALL,locale);

  // Defaults
  int c;
  while((c = getopt(argc,argv,"d:l:s:S:vk1V")) != EOF){
    switch(c){
    case 'V':
      VERSION();
      fprintf(stdout,"Copyright 2023, Clint Turner, KA7OEI\n");
      fprintf(stdout,"Copyright 2023-2024, Rob Robinett, AI6VN\n");
      fprintf(stdout,"%s\n", wd_record_version);
      exit(EX_OK);
    case 'd':
      Recordings = optarg;
      break;
    case 'l':
      locale = optarg;
      break;
    case 'v':
      ++verbosity;
      if ( verbosity > 1 ) {
          fprintf(stderr,"verbosity = %d\n", verbosity);
      }
      break;
    case 'k':
      Keep_wav = 1;
      break;
    case 's':
      Ssrc = strtol(optarg,NULL,0);
      break;
    case 'S':
      Samples_per_second = strtol(optarg,NULL,0);
      break;
    default:
      fprintf(stderr,"Usage: %s [-l locale] [-v] [-k] [-d recdir] [-S samples_per_second] PCM_multicast_address\n",argv[0]);
      exit(1);
      break;
    }
  }
  if(Ssrc == 0){
      fprintf(stderr,"'-s SSRC' must be specified\n");
      exit(1);
  }
  char const *target;
  if(optind >= argc){
    fprintf(stderr,"Specify PCM Multicast IP address or domain name\n");
    exit(1);
  }
  target = argv[optind];
  strlcpy(PCM_mcast_address_text,target,sizeof(PCM_mcast_address_text));
  setlocale(LC_ALL,locale);

  if(strlen(Recordings) > 0 && chdir(Recordings) != 0){
    fprintf(stderr,"Can't change to directory %s: %s, exiting\n",Recordings,strerror(errno));
    exit(1);
  }

  // Set up input socket for multicast data stream from front end
  {
    char iface[1024];
    struct sockaddr sock;
    resolve_mcast(PCM_mcast_address_text,&sock,DEFAULT_RTP_PORT,iface,sizeof(iface),0);
    Input_fd = listen_mcast(&sock,iface);
  }

  if(Input_fd == -1){
    fprintf(stderr,"Can't set up PCM input from %s, exiting\n",PCM_mcast_address_text);
    exit(1);
  }
  int const n = 1 << 20; // 1 MB
  if(setsockopt(Input_fd,SOL_SOCKET,SO_RCVBUF,&n,sizeof(n)) == -1)
    perror("setsockopt");

  // Graceful signal catch
#if 1 // Ignoring child death signals causes system() inside fork() to return errno 10
  signal(SIGCHLD,SIG_IGN); // Don't let children become zombies
#endif
  
  signal(SIGPIPE,closedown);
  signal(SIGINT,closedown);
  signal(SIGKILL,closedown);
  signal(SIGQUIT,closedown);
  signal(SIGTERM,closedown);        
  signal(SIGPIPE,SIG_IGN);

  atexit(cleanup);

  input_loop();

  exit(0);
}

void closedown(int a){
  if ( verbosity > 1 ) {
    fprintf(stderr,"wd-record->closedown(): caught signal %d: %s\n", a, strsignal(a) );
  }
  exit(1);  // Will call cleanup()
}

// Retuns the sample offset of the rtp.timestam or -1 if it is earlier than 
#define WD_MAX_SAMPLES_PER_MINUTE 16000
#define MAX_U32_BIT_VALUE 0xffffffff
#define FIRST_WRAP_SAMPLE ( MAX_U32_BIT_VALUE - WD_MAX_SAMPLES_PER_MINUTE )

uint32_t calculateAbsoluteDifference( uint32_t timestamp, uint32_t first_sample_number){
    uint32_t timestamp_offset = timestamp -  first_sample_number;
    if ( verbosity > 2 ) {
        fprintf(stderr, "calculateAbsoluteDifference(): timestamp=%x - first_sample_number=%x = timestamp_offset=%x\n",  timestamp,  first_sample_number, timestamp_offset);
    }
     return timestamp_offset;
}

struct test_entry {
    uint32_t rs;     // The reference sample, i.e. the sample number of the first sample of a wav file
    uint32_t ts;     // The test sammple, i.e. the sample number carried in the timestamp field of an RTP packet
    int  expected;
};

struct test_entry test_list_fails[] = {
    {0xfffffff0,        100,  116}
};

struct test_entry test_list[] = {
    //  first wav     test        
    {        100,        110,    10 },       // wav starts with sample 100, RTP is timestamp 110 => OK to add to wav file
    {        100,      16100, 16000 },       // wav starts with sample 100, RTP is timestamp 110 => OK to add to wav file
    {        100, 0xffffffff,   -101},
    {        100,        90 ,    -10},
    { 0xfffffff0, 0xffffffff,     15},
    { 0xfffffff0,        100,    116},
    { 0xfffffff0,      16000,  16016},
    { 0xfffffff0, 0xfffff000,  -4080},
    { 0xfffffff0, 0xfffff000,  -4080}
};

void test_calculateAbsoluteDifference() {
    struct test_entry *tep, *tepe;

    for ( tep = &test_list[0], tepe = &test_list[sizeof(test_list)/sizeof(test_list[0])]; tep < tepe; ++tep ) {
        int offset = calculateAbsoluteDifference( tep->ts, tep->rs);
        if ( offset != tep->expected ) {
            printf( "ERROR: reference=%12x, test=%12x  => %d, not the expected %d\n", tep->rs, tep->ts, offset, tep->expected);
        }
    }
}

// Read from RTP network socket, assemble blocks of samples
void input_loop(){
    int64_t loop_count = INT64_MAX - 1;
    int last_flush_second = -1;                // Flush all streams once per second
    int last_data_second = -1;        // Used in search for the first data packet to be put in the first wav fileafter tansition from second 50 to second 0

   test_calculateAbsoluteDifference();
   // exit (0);

    while ( loop_count > 0 ) {        /// In effect this is a forever() loop
        --loop_count;

        // Wait for data or timeout after one second
        fd_set fdset;
        FD_ZERO(&fdset);
        FD_SET(Input_fd,&fdset);        // This macro adds the file descriptor Input_fd to fdset
        {
            // Wait up to one second for data to be avaiable frmm this multicast stream
            struct timespec const polltime = {1, 0}; // return after 1 sec
            int n = pselect(Input_fd + 1,&fdset,NULL,NULL,&polltime,NULL);
            if(n < 0) {
                fprintf(stderr, "input_loop(): ERROR: unexpected pselect() => %d\n.  Timeout waiting for audio from stream", n);
                exit(1); 
            }
        }

        int const current_epoch = utc_time_sec();
        int const current_second   = current_epoch % 60; // UTC second within 0-60 period
        if ( verbosity > 1 && last_flush_second == -1 ) {
            fprintf(stderr, "input_loop(): Starting at second% 2d\n", current_second);
        }

        // Flush the samples to the wav files once each second
        if ( (last_flush_second >= 0 ) && ( current_second != last_flush_second ) ) {
            for(struct session *sp = Sessions; sp != NULL;){
                struct session * const next = sp->next;
                flush_session(&sp);
                sp = next;
            }
        }
        last_flush_second = current_second;

        if(FD_ISSET(Input_fd,&fdset) == 0 ){
            // After waiting for one second we received no packets, so close any open sessions and search for the begining of a new one minute stream 
            if(verbosity > 1) {
                fprintf(stderr, "input_loop(): FD_ISSET() => 0, so close current file and search for start of next minute\n");
            }
            for(struct session *sp = Sessions; sp != NULL;){
                struct session * const next = sp->next;
                close_session(&sp);
                sp = next;
            }
            Searching_for_first_minute = 1;
            continue;
        } else {
            if ( verbosity > 3 ) {
                fprintf(stderr, "input_loop(): FD_ISSET() => %d\n", FD_ISSET(Input_fd,&fdset));
            }
            uint8_t buffer[PKTSIZE];
            socklen_t socksize = sizeof(Sender);
            int size = recvfrom(Input_fd,buffer,sizeof(buffer),0,&Sender,&socksize);
            if(size <= 0){ 
                perror("recvfrom");
                usleep(50000);
                if(verbosity > 0) {
                    fprintf(stderr, "wd-record->input_loop(): ERROR: recvfrom() => %d\n", size);
                }
                continue;
            }

            if(size < RTP_MIN_SIZE) {
                if(verbosity > 0) {
                    fprintf(stderr, "wd-record->input_loop(): ERROR: recvfrom() => %d which is < RTP_MIN_SIZE %d\n", size, RTP_MIN_SIZE);
                }
                continue; 
            }

            struct rtp_header  rtp;
            uint8_t const     *dp = ntoh_rtp(&rtp,buffer);
 
            if(rtp.ssrc != Ssrc) {
                if(verbosity > 3) {
                    fprintf(stderr, "input_loop(): discard data from rtp.ssrc %8d != Ssrc %8d\n", rtp.ssrc, Ssrc);
                }
                ++loop_count;   // So we process loop_count buffers of the SSRC packet stream
                continue;       // We are only processing one SSRC
            }
            if(verbosity > 2) {
                fprintf(stderr, "input_loop(): got a %d byte buffer of SSRC %d data\n", size, Ssrc);
            }

            if(rtp.pad){
                // Remove padding
                size -= dp[size-1];
                rtp.pad = 0;
            }

            if(size <= 0) {
                if(verbosity > 0) {
                    fprintf(stderr, "wd-record->input_loop(): ERROR: rtp buffer size is invalid value %d which is <= 0\n", size);
                }
                continue; // Bogus RTP header
            }
            if( verbosity > 2 ) {
                fprintf(stderr, "input_loop(): rtp buffer size = %d\n",  size );
            } 

            // Find the first session which wants the SSRC or if none in found create a new session 
            struct session *sp;
            for( sp = Sessions; sp != NULL; sp=sp->next)  {
                if(    sp->ssrc == rtp.ssrc
                    && rtp.type == sp->type
                    && address_match( &sp->sender, &Sender )) {
                    if ( verbosity > 2 ) {
                        fprintf(stderr, "input_loop(): found an exisiting session for SSRD %d\n", sp->ssrc);
                    }
                    break;
                }
            }
 
            int sample_offset_in_current_wav_file = -1;
            if ( sp != NULL ) {
                // We have already opened a wav file.  Make sure the samples in this RTP packet are for that file
                if ( Searching_for_first_minute == 1 ) {
                     if ( verbosity > 2 ) {
                        fprintf(stderr, "input_loop(): Got RTP packet with timestamp rtp.timestamp=%u while searching for first second 0\n", rtp.timestamp);
                    }
                } else {
                    sample_offset_in_current_wav_file = calculateAbsoluteDifference( rtp.timestamp, sp->first_sample_number );
                    if ( sample_offset_in_current_wav_file < 0 ) {
                        if ( verbosity > 1 ) {
                            fprintf(stderr, "input_loop(): WARNING: RTP packet with timestamp rtp.timestamp=%u which is less than the timestamp=%u of the first sample of the current wav file. Open new wav file.\n", rtp.timestamp, sp->first_sample_number);
                        }
                        close_session(&sp);
                        sp = NULL;
                        Searching_for_first_minute = 1;
                        continue;
                    }
                    int      samples_per_minute =  sp->samprate * 60;
                    uint32_t sample_number_of_first_sample_of_next_wav_file = sp->first_sample_number + samples_per_minute;
                    if ( sample_offset_in_current_wav_file >= samples_per_minute ) {
                        int sample_offset_in_next_wav_file = sample_offset_in_current_wav_file - samples_per_minute;
                        if ( verbosity > 2 ) {
                            fprintf(stderr, "input_loop(): after writing %7lld samples to wav file which should have %d samples in it, closing wav file because this new rtp packet is for offset %d in the next wav file.  rtp.timestamp=%u >= sample_number_of_first_sample_in_current_wav_file=%u\n",
                                    (long long)sp->SamplesWritten, samples_per_minute, sample_offset_in_next_wav_file, rtp.timestamp, sample_number_of_first_sample_in_current_wav_file );
                        }
                        close_session(&sp);
                        sp = NULL;
                        sample_number_of_first_sample_in_current_wav_file = sample_number_of_first_sample_of_next_wav_file;
                        sample_offset_in_current_wav_file = sample_offset_in_next_wav_file;
                    }
                }
            }
            if ( sp == NULL ) {
                // Open new session for new 1 minute wav fle
                if ( ( Searching_for_first_minute == 0 ) &&  ( current_second != 0 ) ) {
                    if ( verbosity > 0 ) {
                        fprintf(stderr, "wd-record->input_loop(): ERROR: opening new file at second %d, not expected second 0. The RX-888 sample rate is wrong or this server's NTP time is wrong.  Flushing RTP packets until the next second zero\n", current_second);
                    }
                    Searching_for_first_minute = 1;
                    continue;
                }
                sp = create_session(&rtp, current_epoch, rtp.ssrc );	
                if ( sp == NULL ) {
                    if ( verbosity > 0 ) {
                        fprintf(stderr, "wd-record->input_loop(): ERROR: failed to open new wav file\n");
                    }
                    exit(1);
                }   
                sp->first_sample_number = sample_number_of_first_sample_in_current_wav_file;
                if ( verbosity > 2 ) { 
                    fprintf(stderr, "input_loop(): opened new wav file for samples starting at sample #%u which is at wav file offset %d\n", sp->first_sample_number, sample_offset_in_current_wav_file );
                }
            }

            if ( Searching_for_first_minute == 1 ) {
                // We are waiting for the transition from second 59 to second 0 before starting to write data
                if ( verbosity > 2 ) {
                    fprintf(stderr, "input_loop(): searching for first data received in second 0\n");
                }
                if ( last_data_second != 59 ) {
                    // The current second is 0-58, so toss the data
                    if ( verbosity > 2 ) {
                        fprintf(stderr, "input_loop(): tossing data during second %2d while searching for first data received in second 0\n", current_second);
                    }
                    last_data_second = current_second;
                } else {
                    // Last data second was second 59
                    if ( current_second == 59 ) {
                        // This is the second or more packet received during second 59
                        if ( verbosity > 2 ) {
                            fprintf(stderr, "input_loop(): tossing the second or more data packet during second %2d while searching for first data received in second 0\n", current_second);
                        }
                    } else {
                        if ( current_second != 0 ) {
                            // We appear to have missed receiving data during second 0, which would surprise me.
                            if ( verbosity > 2 ) {
                                fprintf(stderr, "input_loop(): ERROR: unexpected transition from second %2d to second %d while missing data during second 0. Start searching for next second 0\n", last_data_second, current_second);
                            }
                            Searching_for_first_minute = 1;
                            continue;
                        } else {
                            sp->first_sample_number = rtp.timestamp;
                            if ( verbosity > 2 ) {
                                fprintf(stderr, "input_loop(): found first data after transition from second 59 to second 0, so sp->first_sample_number=%u.  Record to this wav file until we receive a pkt with timestamp >= %u\n", 
                                        sp->first_sample_number, sp->first_sample_number + ( sp->samprate * 60 ) );
                            }
                            Searching_for_first_minute = 0;
                        }
                    }
                }
            }

            if ( Searching_for_first_minute == 1 ) {
                // We are waiting for the transition from second 59 to second 0 before starting to write data
                if ( verbosity > 2 ) {
                    fprintf(stderr, "input_loop(): dumping data packet.  Search for the next one\n");
                }
            } else {
                if ( verbosity > 2 ) {
                    fprintf(stderr, "input_loop(): recording data packet to wav file\n");
                }
                // A "sample" is a single audio sample, usually 16 bits.
                // A "frame" is the same as a sample for mono. It's two audio samples for stereo
                int16_t const * const samples = (int16_t *)dp;
                size -= (dp - buffer);
                int const samp_count = size / sizeof(*samples); // number of individual audio samples (not frames)
                int const frame_count = samp_count / sp->channels; // 1 every sample period (e.g., 4 for stereo 16-bit)
                off_t const offset = rtp_process(&sp->rtp_state,&rtp,frame_count); // rtp timestamps refer to frames

                // The seek offset relative to the current position in the file is the signed (modular) difference between
                // the actual and expected RTP timestamps. This should automatically handle
                // 32-bit RTP timestamp wraps, which occur every ~1 days at 48 kHz and only 6 hr @ 192 kHz
                // Should I limit the range on this?
                if(offset)
                    fseeko(sp->fp,offset * sizeof(*samples) * sp->channels,SEEK_CUR); // offset is in bytes

                sp->TotalFileSamples += samp_count + offset;
                sp->SamplesWritten += samp_count;

                // Packet samples are in big-endian order; write to .wav file in little-endian order
                for(int n = 0; n < samp_count; n++){
                    fputc(samples[n] >> 8,sp->fp);
                    fputc(samples[n],sp->fp);
                }
            }
        } // end of packet processing
    }     // end of forever loop
}

void cleanup(void){
  while(Sessions){
    // Flush and close each write stream
    // Be anal-retentive about freeing and clearing stuff even though we're about to exit
    struct session * const next_s = Sessions->next;
    fflush(Sessions->fp);
    fclose(Sessions->fp);
    Sessions->fp = NULL;
    FREE(Sessions->iobuffer);
    FREE(Sessions);
    Sessions = next_s;
  }
}

struct session *create_session( 
        struct rtp_header *rtp, 
        const int wav_start_epoch,    // The WD wav file name is derived from the epoch of the first samples of the wav fle 
        const int tuning_freq_hz )
{
    int filename_epoch = wav_start_epoch;
    int  wav_start_second = wav_start_epoch % 60;
    if ( Searching_for_first_minute == 1 ) {
        // If this is the first wav file, then samples will start being written at the begining of the next minute
        // So the filname should reflect that future time
        filename_epoch = wav_start_epoch + 60 - wav_start_second;
        if ( verbosity > 2 ) {
            fprintf( stderr,"create_session(): changing the filename of the first wav file to be derived from epoch=%d rather than from wav_start_epoch=%d\n", filename_epoch, wav_start_epoch);
        }
    } else {
        if ( wav_start_second != 0 ) {
            if( verbosity > 1 ) {
            fprintf( stderr,"create_session(): ERRROR: (INTERNAL) wav_start_epoch=%d is for second %d, not for an expected second 0\n", wav_start_epoch, wav_start_second );
            }
        }
        if( verbosity > 1 ) {
            fprintf( stderr,"create_session(): wav_start_epoch=%d, tuning_freq_hz,%d\n", wav_start_epoch, tuning_freq_hz );
        }
    }
    struct session *sp = calloc(1,sizeof(*sp));
    if ( sp == NULL)  {
        fprintf( stderr,"create_session(): ERROR: can't malloc session pointer\n" );
        return NULL; 
    }

    memcpy(&sp->sender,&Sender,sizeof(sp->sender));
    sp->type = rtp->type;
    sp->ssrc = rtp->ssrc;

    sp->channels = channels_from_pt(sp->type);
    if ( Samples_per_second != 0 ) {
        sp->samprate = Samples_per_second;
    } else {
        sp->samprate = samprate_from_pt(sp->type);
    }

    time_t start_time_sec = filename_epoch;
    struct tm const * const tm = gmtime(&start_time_sec);
    snprintf( sp->filename, sizeof(sp->filename), "%04d%02d%02dT%02d%02d%02dZ_%d_usb.wav",
            tm->tm_year+1900,
            tm->tm_mon+1,
            tm->tm_mday,
            tm->tm_hour,
            tm->tm_min,
            tm->tm_sec,
            tuning_freq_hz)
            ;

    int fd = open(sp->filename,O_RDWR|O_CREAT,0777);
    if(fd == -1){
        if ( verbosity > 0 ) {
            fprintf(stderr,"wd-record->create_session(): ERROR: can't create/write file %s: %s\n",sp->filename,strerror(errno));
        }
        FREE(sp);
        exit(1);
        return NULL;
    }
    // Use fdopen on a file descriptor instead of fopen(,"w+") to avoid the implicit truncation
    // This allows testing where we're killed and rapidly restarted in the same cycle
    sp->fp = fdopen(fd,"w+");
    if( verbosity > 2) {
        fprintf(stderr,"create_session(): creating %s with sample rate of %d\n",sp->filename, sp->samprate);
    }

    assert(sp->fp != NULL);
    // file create succeded, now put us at top of list
    sp->prev = NULL;
    sp->next = Sessions;

    if(sp->next)
        sp->next->prev = sp;

    Sessions = sp;

    sp->iobuffer = malloc(BUFFERSIZE);
    setbuffer(sp->fp,sp->iobuffer,BUFFERSIZE);

    fcntl(fd,F_SETFL,O_NONBLOCK); // Let's see if this keeps us from losing data

    attrprintf(fd,"samplerate","%lu",(unsigned long)sp->samprate);
    attrprintf(fd,"channels","%d",sp->channels);
    attrprintf(fd,"ssrc","%u",rtp->ssrc);
    attrprintf(fd,"sampleformat","s16le");

    // Write .wav header, skipping size fields
    memcpy(sp->header.ChunkID,"RIFF", 4);
    sp->header.ChunkSize = 0xffffffff; // Temporary
    memcpy(sp->header.Format,"WAVE",4);
    memcpy(sp->header.Subchunk1ID,"fmt ",4);
    sp->header.Subchunk1Size = 16;
    sp->header.AudioFormat = 1;
    sp->header.NumChannels = sp->channels;
    sp->header.SampleRate = sp->samprate;

    sp->header.ByteRate = sp->samprate * sp->channels * 16/8;
    sp->header.BlockAlign = sp->channels * 16/8;
    sp->header.BitsPerSample = 16;
    memcpy(sp->header.SubChunk2ID,"data",4);
    sp->header.Subchunk2Size = 0xffffffff; // Temporary
    fwrite(&sp->header,sizeof(sp->header),1,sp->fp);
    fflush(sp->fp); // get at least the header out there

    attrprintf(fd,"source","%s",formatsock(&Sender,false));
    attrprintf(fd,"multicast","%s",PCM_mcast_address_text);
    attrprintf(fd,"unixstarttime","%.9lf",(double)filename_epoch);

    return sp;
}

void flush_session(struct session **p){
  if(p == NULL)
    return;
  struct session *sp = *p;
  if(sp == NULL)
    return;

  if ( (verbosity > 2) && (sp->SamplesWritten != 0) )
    fprintf(stderr, "flush_session(): Flushing %s %'.1f/%'.1f sec\n",sp->filename,
	   (float)sp->SamplesWritten / sp->samprate,
	   (float)sp->TotalFileSamples / sp->samprate);
  
  if(sp->fp != NULL){
    // Get final file size, write .wav header with sizes
    fflush(sp->fp);
  }
  return;
}
 
void close_session(struct session **p){
  if(p == NULL)
    return;
  struct session *sp = *p;
  if(sp == NULL)
    return;

  if(verbosity > 2)
    fprintf(stderr,"close_session(): closing %s %'.1f/%'.1f sec\n",sp->filename,
	   (float)sp->SamplesWritten / sp->samprate,
	   (float)sp->TotalFileSamples / sp->samprate);
  
  if(sp->fp != NULL){
    // Get final file size, write .wav header with sizes
    fflush(sp->fp);
    struct stat statbuf;
    fstat(fileno(sp->fp),&statbuf);
    sp->header.ChunkSize = statbuf.st_size - 8;
    sp->header.Subchunk2Size = statbuf.st_size - sizeof(sp->header);
    rewind(sp->fp);
    fwrite(&sp->header,sizeof(sp->header),1,sp->fp);
    fflush(sp->fp);
    fclose(sp->fp);
    sp->fp = NULL;
  }
  FREE(sp->iobuffer);
  if(sp->prev)
    sp->prev->next = sp->next;
  else
    Sessions = sp->next;
  if(sp->next)
    sp->next->prev = sp->prev;
  FREE(sp);
  *p = NULL;
}
