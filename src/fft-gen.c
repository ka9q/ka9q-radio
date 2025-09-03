#define _GNU_SOURCE 1
#include <assert.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdbool.h>
#include <pthread.h>
#include <memory.h>
#include <complex.h>
#include <math.h>
#include <fftw3.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <getopt.h>
#include <sys/stat.h>
#include <errno.h>
#include <sys/file.h>

#define System_wisdom_file "/etc/fftw/wisdomf"
#define Wisdom_dir "/var/lib/ka9q-radio"
#define Wisdom_file "/var/lib/ka9q-radio/wisdom"

static int Verbose;
static bool Force;

static struct {
  char const *name;
  int level;
} Levels[] = { // Low to high
  {"estimate", FFTW_ESTIMATE},
  {"measure", FFTW_MEASURE},
  {"patient", FFTW_PATIENT},
  {"exhaustive", FFTW_EXHAUSTIVE},
  {NULL, -1},
};

static char const * level_to_name(int x){
  for(int i = 0;; i++){
    if(Levels[i].level == -1)
      return NULL;

    if(Levels[i].level == x)
      return Levels[i].name;
  }
}

static int name_to_level(char const *name){
  for(int i = 0;; i++){
    if(Levels[i].level == -1)
      return 0;
    if(strcasecmp(Levels[i].name,name) == 0)
      return Levels[i].level;
  }
}
static int save_plans();
static size_t Wisdom_size;
static uint64_t Wisdom_hash;

static int track_wisdom_length(void){
  if(Verbose < 2)
    return 0;

  size_t length= 0;
  char *wisdom = fftwf_export_wisdom_to_string();
  if(wisdom != NULL)
    length = strlen(wisdom);

  uint64_t h = 0xcbf29ce484222325ULL;

  for (size_t i = 0; i < length; ++i) {
    h ^= (unsigned char)wisdom[i];
    h *= 0x100000001b3ULL;
  }
  if(length != Wisdom_size || Wisdom_hash !=h){
    printf("wisdom changed (grew %lu)\n",(unsigned long)(length - Wisdom_size));
    Wisdom_size = length;
    Wisdom_hash = h;
  }
  free(wisdom); wisdom = NULL;
  return length;
}
void usage(){
  printf("fft-gen: creates and updates ffw3f wisdom for ka9q-radio\n");
  printf("usage: fft-gen [-h] [-v|--verbose [-v|--verbose]] [--timelimit|-t sec] [--threads|-T <n>] [--force|-f] [--patient|--measure|--estimate|--exhaustive|-x|-e|-m|-p] transform...\n");
  printf("  eg   fft-gen -v --exhaustive cob200 cob300 cob400 cob600 cob1200\n");
}



static char Optstring[] = "hepmxT:t:vf";
static struct option Options[] = {
  {"help", no_argument, NULL, 'h'},
  {"timelimit", required_argument, NULL, 't'},
  {"patient", no_argument, NULL, 'p'},
  {"measure", no_argument, NULL, 'm'},
  {"exhaustive", no_argument, NULL, 'x'},
  {"estimate", no_argument, NULL, 'e'},
  {"threads", required_argument, NULL, 'T'},
  {"verbose", no_argument, NULL, 'v'},
  {"force", no_argument, NULL, 'f'},
  {NULL, 0, NULL, 0},
};

double FFTW_plan_timelimit = 0;
int FFTW_planning_level = FFTW_PATIENT;


static int parse_and_run(char *s);

// Experiment with incremental wisdom generation
int main(int argc,char *argv[]){
  int c;
  bool real = false;
  int nthreads = 1;
  int direction = FFTW_FORWARD;

  (void)name_to_level(""); // shut up compiler
  while((c = getopt_long(argc,argv,Optstring,Options,NULL)) != -1){
    switch(c){
    case 'h':
    default:
      usage();
      exit(0);
    case 'v':
      Verbose++;
      break;
    case 't':
      FFTW_plan_timelimit = strtod(optarg,NULL);
      break;
    case 'p':
      FFTW_planning_level = FFTW_PATIENT;
      break;
    case 'm':
      FFTW_planning_level = FFTW_MEASURE;
      break;
    case 'x':
      FFTW_planning_level = FFTW_EXHAUSTIVE;
      break;
    case 'e':
      FFTW_planning_level = FFTW_ESTIMATE;
      break;
    case 'T':
      nthreads = strtol(optarg,NULL,0);
      break;
    case 'f':
      Force = true;
      break;
    }
  }
  if(Verbose && real && direction == FFTW_BACKWARD){
    printf("direction ignored on real->complex transforms\n");
    direction = FFTW_FORWARD;
  }

  if(Verbose > 1)
    printf("FFTW version: %s\n", fftwf_version);
  fftwf_init_threads();
  fftwf_plan_with_nthreads(nthreads);

  bool sr = false;
  bool lr = false;
  if(Force){
    if(Verbose > 1)
      printf("Not loading wisdom\n");
  } else {
    sr = fftwf_import_system_wisdom();
    if(Verbose > 1)
      printf("fftwf_import_system_wisdom() %s\n",sr ? "succeeded" : "failed");
    if(!sr && Verbose){
      if(access(System_wisdom_file,R_OK) == -1){ // Would really like to use AT_EACCESS flag
	printf("%s not readable: %s\n",System_wisdom_file,strerror(errno));
      }
    }
    track_wisdom_length();
    lr = fftwf_import_wisdom_from_filename(Wisdom_file);
    if(Verbose > 1){
      printf("fftwf_import_wisdom_from_filename(%s) %s\n",Wisdom_file,lr ? "succeeded" : "failed");
      if(!lr){
	if(access(Wisdom_file,R_OK) == -1){
	  printf("%s not readable: %s\n",Wisdom_file,strerror(errno));
	}
      }
      if(access(Wisdom_file,W_OK) == -1)
	printf("Warning: %s not writeable, exports will fail: %s\n",Wisdom_file,strerror(errno));
    }
    track_wisdom_length();
  }
  if(Verbose > 1 && !sr && !lr)
    printf("No wisdom read\n");

  if(Verbose > 1){
    printf("nthreads = %d, level = %s",
	   nthreads,
	   level_to_name(FFTW_planning_level));

    if(FFTW_plan_timelimit !=0)
      printf(", time limit %.1lf sec\n",FFTW_plan_timelimit);
    else
      printf(", no time limit\n");
  }
  if(optind < argc){
    for(int i=optind; i < argc; i++)
      parse_and_run(argv[i]);
  } else {
    // read from stdin
    char buffer[1024];
    while(fgets(buffer,sizeof buffer,stdin) != NULL)
      parse_and_run(buffer);
  }
  exit(0);
}
static int parse_and_run(char *s){
  char *cp = strchr(s,'\n');
  if(cp)
    *cp = '\0'; // chomp newline

  // Parse
  char a1,a2,a3;
  int a4;
  if(sscanf(s,"%c%c%c%d",&a1,&a2,&a3,&a4) != 4){
    printf("Can't parse %s\n",s);
    return -1;
  }

  bool real = false;
  int direction = FFTW_FORWARD;
  int N = 1024;

  switch(a1){
  case 'c':
    real = false;
    break;
  case 'r':
    real = true;
    break;
  default:
    printf("Unknown type %c\n",a1);
    return -1;
  }
  bool inplace = (a2 == 'i');

  switch(a3){
  case 'f':
    direction = FFTW_FORWARD;
    break;
  case 'b':
    direction = FFTW_BACKWARD;
    break;
  default:
    printf("Unknown direction %c\n",a3);
    return -1;
  }
  if(a4 <= 0){
    printf("invalid length %d\n",a4);
    return -1;
  }
  N = a4;
  if(Verbose)
    printf("%s\n",s);


  fftwf_plan plan = NULL;
  if(FFTW_plan_timelimit != 0)
    fftwf_set_timelimit(FFTW_plan_timelimit);

  if(real && direction == FFTW_FORWARD){
    float *in = fftwf_malloc(N * sizeof(float));
    float complex *out = (float complex *)in;
    if(!inplace)
      out = fftwf_malloc(N * sizeof(float complex));
    plan = fftwf_plan_dft_r2c_1d(N, in, out, FFTW_planning_level | (!inplace ? FFTW_PRESERVE_INPUT : 0));
    if((void *)out != (void *)in)
      fftwf_free(out);
    fftwf_free(in);
  } else if(real && direction == FFTW_BACKWARD){
    float complex *in = fftwf_malloc(N * sizeof(float complex));
    float *out = (float *)in;
    if(!inplace)
      out = fftwf_malloc(N * sizeof(float));
    plan = fftwf_plan_dft_c2r_1d(N, in, out, FFTW_planning_level | (!inplace ? FFTW_PRESERVE_INPUT : 0));
    if((void *)out != (void *)in)
      fftwf_free(out);
    fftwf_free(in);
  } else {
    // Complex
    float complex *in = fftwf_malloc(N * sizeof(float complex));
    float complex *out = in;
    if(!inplace)
      out = fftwf_malloc(N * sizeof(float complex));
    plan = fftwf_plan_dft_1d(N, in, out, direction, FFTW_planning_level | (!inplace ? FFTW_PRESERVE_INPUT : 0));
    if((void *)out != (void *)in)
      fftwf_free(out);
    fftwf_free(in);
  }
  if(plan != NULL){
    fftwf_destroy_plan(plan);
    plan = NULL;
    save_plans();
  }
  track_wisdom_length();
  return 0;
}
static int save_plans(){
  // Do all this carefully to avoid losing old (or new) wisdom
  char *newtemp = NULL;
  char *lockfile = NULL;
  char *wisdom = NULL;
  int lockfd = -1;
  int fd = -1;

  // Import or re-import wisdom and merge
  if(asprintf(&lockfile,"%s.lock",Wisdom_file) <= 0)
    goto quit;

  lockfd = open(lockfile,O_CREAT|O_RDWR,0664);
  fchmod(lockfd,0664); // I really do want rw-rw-r-- so the radio group can write it
  if(lockfd == -1)
    printf("Can't acquire lock on %s\n",lockfile);
  else
    flock(lockfd,LOCK_EX);
  bool reimport = fftwf_import_wisdom_from_filename(Wisdom_file);
  if(Verbose > 1)
    printf("fftwf_import_wisdom_from_filename(%s) %s\n",Wisdom_file,reimport ? "succeeded" : "failed");
  track_wisdom_length();

  if(asprintf(&newtemp,"%s-XXXXXX",Wisdom_file) < 0)
    goto quit;

  wisdom = fftwf_export_wisdom_to_string();
  ssize_t newsize = strlen(wisdom);

  fd = mkstemp(newtemp);
  if(fd == -1){
    // Last ditch attempt to preserve the work: dump it to stdout and hope somebody sees it
    printf("Can't create temporary wisdom file %s\n",newtemp);
    printf("New wisdom:\n");
    int r = write(1,wisdom,newsize); // Best effort, ignore return
    (void)r;
    goto quit;
  }
  if(write(fd,wisdom,newsize) != newsize){
    printf("Write of new wisdom file failed: %s\n",strerror(errno));
    goto quit;
  }
  if(lockfd == -1)
    // Lock wasn't acquired, don't try to merge. Just leave temp file to be manually cleaned up (don't waste the work)
    goto quit;

  // Copy user/group/mode from old version
  struct stat st;
  if(stat(Wisdom_file,&st) == 0){
    int r = fchown(fd, st.st_uid, st.st_gid); // Best effort
    (void)r;
    fchmod(fd, st.st_mode);
  }
  if(fstat(fd,&st) != 0 || st.st_size <= 0){
    printf("New wisdom file %s missing or empty!\n",newtemp);
    goto quit;
  }
  // Make sure it's really, really out there
  fsync(fd);
  close(fd);
  fd = -1;
  int dir = open(Wisdom_dir,O_DIRECTORY);
  fsync(dir);
  close(dir);
  if(rename(newtemp,Wisdom_file) != 0)
    printf("rename %s to %s failed: %s\n",newtemp,Wisdom_file,strerror(errno));
  else if(Verbose > 1)
    printf("rename %s to %s succeeded\n",newtemp,Wisdom_file);

  if(Force)
    fftwf_forget_wisdom(); // start fresh for the next on the list

  quit:

  if(lockfd != -1){
    flock(lockfd,LOCK_UN);
    close(lockfd);
    lockfd = -1;
  }
  if(fd != -1)
    close(fd);
  free(wisdom);
  free(newtemp);
  free(lockfile);
  return 0;
}
