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

int Verbose;
bool Force;

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
int plan(int level, int direction, int real, int N, double limit);

static size_t Wisdom_size;
uint64_t Wisdom_hash;

static int track_wisdom_length(void){
  if(Verbose < 2)
    return 0;

  char *wisdom = fftwf_export_wisdom_to_string();
  size_t length = strlen(wisdom);

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


static char Optstring[] = "epmxT:t:vf";
static struct option Options[] = {
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

// Experiment with incremental wisdom generation
int main(int argc,char *argv[]){
  int c;
  int N = 2000000;
  bool real = false;
  double FFTW_plan_timelimit = 0;  
  int FFTW_planning_level = FFTW_PATIENT;
  int nthreads = 1;
  int direction = FFTW_FORWARD;

  (void)name_to_level(""); // shut up compiler
  while((c = getopt_long(argc,argv,Optstring,Options,NULL)) != -1){
    switch(c){
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
    default:
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
  for(int i=optind; i < argc; i++){
    // Parse
    char a1,a2,a3;
    int a4;
    if(sscanf(argv[i],"%c%c%c%d",&a1,&a2,&a3,&a4) != 4){
      printf("Can't parse %s\n",argv[i]);
      continue;
    }
    switch(a1){
    case 'r':
      real = true;
      break;
    case 'c':
      real = false;
      break;
    default:
      printf("Unknown type %c\n",a1);
      continue;
    }
    if(a2 != 'o'){
      printf("Only out-of-place (o) handled: %s\n",argv[i]);
      continue;
    }
    switch(a3){
    case 'f':
      direction = FFTW_FORWARD;
      break;
    case 'b':
      direction = FFTW_BACKWARD;
      break;
    default:
      printf("Unknown direction %c\n",a3);
      continue;
    }
    if(a4 <= 0){
      printf("invalid length %d\n",a4);
      continue;
    }
    N = a4;

    if(Verbose)
      printf("%s\n",argv[i]);

    plan(FFTW_planning_level, direction, real, N, FFTW_plan_timelimit);
  }
  exit(0);
}

int plan(int level, int direction, int real, int N, double limit){
  float * inr = fftwf_malloc(N * sizeof(float));
  float complex * in = fftwf_malloc(N * sizeof (float complex));
  float complex * out = fftwf_malloc(N * sizeof (float complex));
  
  fftwf_plan plan = NULL;
  if(limit != 0)
    fftwf_set_timelimit(limit);

  if(real && direction == FFTW_FORWARD){
    plan = fftwf_plan_dft_r2c_1d(N, inr, out, level | FFTW_PRESERVE_INPUT);
  } else if(real && direction == FFTW_BACKWARD){
    plan = fftwf_plan_dft_c2r_1d(N, out, inr, level | FFTW_PRESERVE_INPUT);
  } else {
    // Complex
    plan = fftwf_plan_dft_1d(N, in, out, direction, level | FFTW_PRESERVE_INPUT);
  }
  if(plan != NULL){
    fftwf_destroy_plan(plan);
    plan = NULL;
    save_plans();
  }
  track_wisdom_length();
  fftwf_free(inr);
  inr = NULL;
  fftwf_free(in);
  in = NULL;
  fftwf_free(out);
  out = NULL;
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

  lockfd = open(lockfile,O_CREAT|O_RDWR,0666);
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
    write(1,wisdom,newsize);
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
    fchown(fd, st.st_uid, st.st_gid);
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
