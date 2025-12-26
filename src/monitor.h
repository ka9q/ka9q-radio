#include <stdatomic.h>
#include <samplerate.h>


#define MAX_MCAST 20          // Maximum number of multicast addresses
#define BUFFERSIZE (1<<17)    // about 2.73 sec at 48 kHz - must be power of 2 times page size (4k)!
#define NSESSIONS 1000

#define N_tones 55

// Add two numbers modulo the buffer size
// Used frequently for writin into output buffers with non-wrapping counters
#define BINDEX(a,b) (((a) + (b)) & (BUFFERSIZE - 1))


// Bounce buffer size = 120 ms @ 48 kHz stereo (biggest Opus packet)
// Enlarge this if sample rates > 48 kHz are ever used
#define BBSIZE (2*5760)
struct session {
  _Atomic bool inuse;
  bool initialized;
  struct sockaddr_storage sender;
  char const *dest;

  float *bounce;           // Scratch sample buffer for current packet.
  float *buffer;           // Output stream, read by Portaudio callback
  SRC_STATE *src_state_mono;
  SRC_STATE *src_state_stereo;

  _Atomic int64_t wptr;    // Next write sample, in output sample clock units

  int64_t consec_erasures;
  int consec_lates;
  int consec_out_of_order;

  pthread_t task;           // Thread reading from queue and running decoder
  struct packet *queue;     // Incoming RTP packets
  pthread_mutex_t qmutex;   // Mutex protecting packet queue
  pthread_cond_t qcond;     // Condition variable for arrival of new packet

  struct rtp_state rtp_state; // Incoming RTP session state
  uint32_t ssrc;            // RTP Sending Source ID
  int type;                 // RTP type (10,11,20,111,etc)
  struct pt_table pt_table[128];     // convert a payload type to samplerate, channels, encoding type

  uint32_t next_timestamp;  // Next timestamp expected
  int playout;              // Initial playout delay, frames
  int64_t last_active;    // GPS time last active with data traffic
  double tot_active;         // Total PCM time, s
  double active;             // Seconds we've been active (only when queue has stuff)
  double datarate;           // Smoothed channel data rate

  OpusDecoder *opus;        // Opus codec decoder handle, if needed
  int opus_channels;        // Actual channels in Opus stream
  int frame_size;
  int bandwidth;            // Audio bandwidth
  struct goertzel tone_detector[N_tones];
  int tone_samples;
  double current_tone;       // Detected tone frequency
  double snr;                // Extracted from status message from radiod
  double level;              // Smoothed audio power

  int samprate;
  int channels;              // channels on stream (1 or 2). Opus is always stereo
  double gain;               // linear gain; 1 = 0 dB
  double pan;                // Stereo position: 0 = center; -1 = full left; +1 = full right

  // Counters
  uint64_t packets;    // RTP packets for this session
  uint64_t empties;    // RTP but no data
  uint64_t lates;
  uint64_t resets;
  uint64_t reseqs;
  uint64_t plcs;       // Opus packet loss conceals

  _Atomic bool terminate;            // Set to cause thread to terminate voluntarily
  bool muted;                // Do everything but send to output
  bool reset;                // Set to force output timing reset on next packet
  bool running;              // Audio arrived recently

  char id[32];
  bool notch_enable;         // Enable PL removal notch
  struct iir iir_left;       // State for PL removal filter
  struct iir iir_right;
  double notch_tone;
  struct channel chan;       // Partial copy of radiod's channel structure, filled in by status protocol
  struct frontend frontend;  // Partial copy of radiod's front end structure, ditto
};


// Names of config file sections
extern char const *Radio;
extern char const *Audio;
extern char const *Repeater;
extern char const *Display;

// Command line/config file/interactive command parameters
extern char const *Tx_on;
extern char const *Tx_off;
extern int DAC_samprate;   // Actual hardware output rate
extern int Update_interval;  // Default time in ms between display updates
extern char const *App_path;
extern int Verbose;                       // Verbosity flag
extern char const *Config_file;
extern bool Quiet;                 // Disable curses
extern bool Quiet_mode;            // Toggle screen activity after starting
extern double Playout;
extern bool Constant_delay;
extern bool Start_muted;
extern bool Auto_position;  // first will be in the center
extern double Repeater_tail;
extern char const *Cwid; // Make this configurable!
extern double ID_pitch;
extern double ID_level;
extern double ID_speed;
extern double Gain; // unity gain by default
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
extern char const *Source; // Only accept from this domain name

// Global variables that regularly change
extern double const Latency; // chunk size for audio output callback
extern double const Tone_period; // PL tone integration period
extern double PL_tones[N_tones];

extern int64_t Last_xmit_time;
extern int64_t Last_id_time;
extern _Atomic unsigned Callback_quantum; // How much the callback reads at a time
extern _Atomic uint64_t Output_time;  // Output sample clock, frames (48 kHz)
extern volatile bool PTT_state;      // For repeater transmitter
extern _Atomic uint64_t Audio_frames;
extern _Atomic int64_t LastAudioTime;
extern _Atomic uint64_t Output_total;
extern _Atomic uint64_t Callbacks;
extern _Atomic double Output_level; // Output level, mean square
extern double Portaudio_delay;
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

extern _Atomic bool Terminate;
extern bool Voting;
extern struct session *Best_session; // Session with highest SNR
extern struct sockaddr Metadata_dest_socket;
extern char const *Pipe;
extern struct sockaddr_in *Source_socket;

extern struct session Sessions[NSESSIONS];

void load_id(void);
void cleanup(void);
void *display(void *);

struct session *lookup_or_create_session(struct sockaddr_storage const *,uint32_t);
int close_session(struct session *);
int pa_callback(void const *,void *,unsigned long,PaStreamCallbackTimeInfo const *,PaStreamCallbackFlags,void *);
void *decode_task(void *x);
void *dataproc(void *arg);
void *statproc(void *arg);
void *repeater_ctl(void *arg);
char const *lookupid(double freq,double tone);
bool kick_output();
void vote();
void reset_playout(struct session *sp);

static inline int modsub(int a, int b, int const modulus){
  if(a >= modulus)
    a %= modulus;
  if(b >= modulus)
    b %= modulus;

  int diff = a - b;
  if(diff > modulus/2)
    return diff - modulus;
  if(diff < -modulus/2)
    return diff + modulus;
  return diff;
}
