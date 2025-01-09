#define MAX_MCAST 20          // Maximum number of multicast addresses
#define BUFFERSIZE (1<<19)    // about 10.92 sec at 48 kHz - must be power of 2 times page size (4k)!
extern float const Latency; // chunk size for audio output callback
extern float const Tone_period; // PL tone integration period
#define NSESSIONS 1500

#define N_tones 55
extern float PL_tones[N_tones];


// Names of config file sections
extern char const *Radio;
extern char const *Audio;
extern char const *Repeater;
extern char const *Display;

// Command line/config file/interactive command parameters
extern char const *Tx_on;
extern char const *Tx_off;
extern unsigned int DAC_samprate;   // Actual hardware output rate
extern int Update_interval;  // Default time in ms between display updates
extern char const *App_path;
extern int Verbose;                       // Verbosity flag
extern char const *Config_file;
extern bool Quiet;                 // Disable curses
extern bool Quiet_mode;            // Toggle screen activity after starting
extern float Playout;
extern bool Constant_delay;
extern bool Start_muted;
extern bool Auto_position;  // first will be in the center
extern int64_t Repeater_tail;
extern char const *Cwid; // Make this configurable!
extern double ID_pitch;
extern double ID_level;
extern double ID_speed;
extern float Gain; // unity gain by default
extern bool Notch;
extern char *Mcast_address_text[]; // Multicast address(es) we're listening to
extern char const *Audiodev;    // Name of audio device; empty means portaudio's default
extern int Position; // auto-position streams
extern bool Auto_sort;
// IDs must be at least every 10 minutes per FCC 97.119(a)
extern int64_t Mandatory_ID_interval;
// ID early when carrier is about to drop, to avoid stepping on users
extern int64_t Quiet_ID_interval;
extern int Dit_length;
extern int Channels;
extern char const *Init;

// Global variables that regularly change
extern int64_t Last_xmit_time;
extern int64_t Last_id_time;
extern float *Output_buffer;
extern int Buffer_length; // Bytes left to play out, max BUFFERSIZE
extern volatile unsigned int Rptr;   // callback thread read pointer, *frames*
extern volatile unsigned int Wptr;   // For monitoring length of output queue
extern volatile bool PTT_state;      // For repeater transmitter
extern uint64_t Audio_callbacks;
extern unsigned long Audio_frames;
extern volatile int64_t LastAudioTime;
extern int32_t Portaudio_delay;
extern pthread_t Repeater_thread;
extern pthread_cond_t PTT_cond;
extern pthread_mutex_t PTT_mutex;
extern int Nfds;                     // Number of input multicast streams
extern pthread_mutex_t Sess_mutex;
extern PaStream *Pa_Stream;          // Portaudio stream handle
extern int inDevNum;                 // Portaudio's audio output device index
extern int64_t Start_time;
extern pthread_mutex_t Stream_mutex; // Control access to stream start/stop
extern PaTime Start_pa_time;
extern PaTime Last_callback_time;
extern int Invalids;
extern int64_t Last_error_time;
extern int Nsessions;
extern struct session *Sessions[NSESSIONS];
extern bool Terminate;
extern bool Voting;
extern struct session *Best_session; // Session with highest SNR
extern struct sockaddr_storage Metadata_dest_socket;
extern pthread_mutex_t Rptr_mutex;
extern pthread_cond_t Rptr_cond;


extern int Mcast_ttl;

struct session {
  bool init;               // Fully initialized by first RTP packet
  struct sockaddr_storage sender;
  char const *dest;

  pthread_t task;           // Thread reading from queue and running decoder
  struct packet *queue;     // Incoming RTP packets
  pthread_mutex_t qmutex;   // Mutex protecting packet queue
  pthread_cond_t qcond;     // Condition variable for arrival of new packet

  struct rtp_state rtp_state; // Incoming RTP session state
  uint32_t ssrc;            // RTP Sending Source ID
  int type;                 // RTP type (10,11,20,111,etc)
  struct pt_table pt_table[128];     // convert a payload type to samplerate, channels, encoding type

  uint32_t next_timestamp;  // Next timestamp expected
  unsigned int wptr;        // current write index into output PCM buffer, *frames*
  int playout;              // Initial playout delay, frames
  long long last_active;    // GPS time last active with data traffic
  float tot_active;         // Total PCM time, ns
  float active;             // Seconds we've been active (only when queue has stuff)
  float datarate;           // Smoothed channel data rate

  OpusDecoder *opus;        // Opus codec decoder handle, if needed
  int opus_channels;        // Actual channels in Opus stream
  int frame_size;
  int bandwidth;            // Audio bandwidth
  struct goertzel tone_detector[N_tones];
  int tone_samples;
  float current_tone;       // Detected tone frequency
  float snr;                // Extracted from status message from radiod

  unsigned int samprate;
  unsigned int channels;    // channels on stream (1 or 2). Opus is always stereo
  float gain;               // linear gain; 1 = 0 dB
  float pan;                // Stereo position: 0 = center; -1 = full left; +1 = full right

  // Counters
  unsigned long packets;    // RTP packets for this session
  unsigned long empties;    // RTP but no data
  unsigned long lates;
  unsigned long earlies;
  unsigned long resets;
  unsigned long reseqs;
  unsigned long spares;     // spare counter for debugging

  bool terminate;            // Set to cause thread to terminate voluntarily
  bool muted;                // Do everything but send to output
  bool reset;                // Set to force output timing reset on next packet
  bool now_active;           // Audio arrived < 500 ms ago; updated by vote()

  char id[32];
  bool notch_enable;         // Enable PL removal notch
  struct iir iir_left;       // State for PL removal filter
  struct iir iir_right;
  float notch_tone;
  struct channel chan;       // Partial copy of radiod's channel structure, filled in by status protocol
  struct frontend frontend;  // Partial copy of radiod's front end structure, ditto
};

void load_id(void);
void cleanup(void);
void *display(void *);
void reset_session(struct session *sp,uint32_t timestamp);
struct session *lookup_or_create_session(struct sockaddr_storage const *,uint32_t);
int close_session(struct session **);
int pa_callback(void const *,void *,unsigned long,PaStreamCallbackTimeInfo const *,PaStreamCallbackFlags,void *);
void *decode_task(void *x);
void *dataproc(void *arg);
void *statproc(void *arg);
void *repeater_ctl(void *arg);
char const *lookupid(double freq,float tone);
bool kick_output();
void vote();

static inline int modsub(unsigned int const a, unsigned int const b, int const modulus){
  int diff = (int)a - (int)b;
  if(diff > modulus)
    return diff % modulus; // Unexpectedly large, just do it the slow way

  if(diff > modulus/2)
   return  diff - modulus;

  if(diff < -modulus)
    return diff % modulus; // Unexpectedly small

  if(diff < -modulus/2)
    diff += modulus;

  return diff;
}
static inline struct session *sptr(int index){
  if(index >= 0 && index < Nsessions && !Sessions[index]->terminate)
    return Sessions[index];
  return NULL;
}
