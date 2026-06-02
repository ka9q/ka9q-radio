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
#include <limits.h>

#include "misc.h"
#include "config_paths.h"

#define SYSTEM_WISDOM_FILE "/etc/fftw/wisdomf"

#ifndef STATEDIR
#define STATEDIR "/var/lib/ka9q-radio"
#endif
#define GENERIC_WISDOM_FILE STATEDIR "/wisdom"

static char const * level_to_name(int x);
static int name_to_level(char const *name);
static void load_wisdom(void);
static int save_wisdom(char const *wisdom_file);
static void usage(void);
static int parse_and_run(char *s);

static size_t Wisdom_size;
char *Wisdom_string;
static bool Force;
int Nthreads = 1;

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

char *Arch_wisdom_file; // can't be static, needs FFTW library version
int Verbose;

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
int FFTW_planning_level = FFTW_PATIENT; // Default


// Experiment with incremental wisdom generation
int main(int argc,char *argv[]){
  (void)name_to_level(""); // shut up compiler
  int c;
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
      Nthreads = atoi(optarg);
      break;
    case 'f':
      Force = true;
      break;
    }
  }
  if(Verbose)
    printf("FFTW version: %s\n", fftwf_version);
  fftwf_init_threads();
  fftwf_plan_with_nthreads(Nthreads);

  if(Verbose > 1){
    printf("nthreads = %d, level = %s",
	   Nthreads,
	   level_to_name(FFTW_planning_level));

    if(FFTW_plan_timelimit !=0)
      printf(", time limit %.1lf sec\n",FFTW_plan_timelimit);
    else
      printf(", no time limit\n");
  }
  int n = asprintf(&Arch_wisdom_file, "%s-%s-threaded", GENERIC_WISDOM_FILE, fftwf_version);
  if(Arch_wisdom_file == NULL || n < 0){
    printf("Can't construct arch wisdom file name\n");
    goto bomb;
  }
  // make sure we can write it
  if(0 != save_wisdom(Arch_wisdom_file))
    goto bomb;
  if(optind < argc){
    for(int i=optind; i < argc; i++)
      parse_and_run(argv[i]);
  } else {
    // read from stdin
    char buffer[1024];
    while(fgets(buffer,sizeof buffer,stdin) != NULL)
      parse_and_run(buffer);
  }
 bomb:;
  FREE(Arch_wisdom_file);
  exit(0);
}
static int parse_and_run(char *s){
  {
    char *cp = strchr(s,'\n');
    if(cp)
      *cp = '\0'; // chomp newline
  }
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
  if(!Force)
    load_wisdom();
  else if(Verbose > 1)
    printf("Not loading wisdom\n");

  if(FFTW_plan_timelimit != 0)
    fftwf_set_timelimit(FFTW_plan_timelimit);

  if(Verbose)
    printf("%s\n",s);

  fftwf_plan plan = NULL;
  if(!real){
    // Complex
    float complex *in = fftwf_malloc(N * sizeof(float complex));
    if(inplace){
      plan = fftwf_plan_dft_1d(N, in, in, direction, FFTW_planning_level);
    } else {
      float complex *out = fftwf_malloc(N * sizeof(float complex));
      plan = fftwf_plan_dft_1d(N, in, out, direction, FFTW_planning_level | FFTW_PRESERVE_INPUT);
      fftwf_free(out);
    }
    fftwf_free(in);
  } else if(direction == FFTW_FORWARD){
    float *in = fftwf_malloc(N * sizeof(float));
    if(inplace){
      plan = fftwf_plan_dft_r2c_1d(N, in, (float complex *)in, FFTW_planning_level);
    } else {
      float complex *out = fftwf_malloc(N * sizeof(float complex));
      plan = fftwf_plan_dft_r2c_1d(N, in, out, FFTW_planning_level | FFTW_PRESERVE_INPUT);
      fftwf_free(out);
    }
    fftwf_free(in);
  } else {
    // FFTW_BACKWARD
    float complex *in = fftwf_malloc(N * sizeof(float complex));
    if(inplace){
      plan = fftwf_plan_dft_c2r_1d(N, in, (float *)in, FFTW_planning_level);
    } else {
      float *out = fftwf_malloc(N * sizeof(float));
      plan = fftwf_plan_dft_c2r_1d(N, in, out, FFTW_planning_level | FFTW_PRESERVE_INPUT);
      fftwf_free(out);
    }
    fftwf_free(in);
  }
  if(plan != NULL){
    fftwf_destroy_plan(plan);
    plan = NULL;
    // Import or re-import wisdom and merge
    save_wisdom(Arch_wisdom_file);
  }
  return 0;
}
// Do all this carefully to avoid losing old (or new) wisdom
static int save_wisdom(char const *wisdom_file){
  // Try reimporting arch file again in case it was written to while we were planning
  bool reimport = fftwf_import_wisdom_from_filename(Arch_wisdom_file);
  if(Verbose > 1)
    printf("fftwf_import_wisdom_from_filename(%s) %s\n",Arch_wisdom_file,reimport ? "succeeded" : "failed");

  // Export the merged old+new wisdom
  char *wisdom = fftwf_export_wisdom_to_string();
  if(wisdom == NULL){
    printf("fftwf_export_wisdom_to_string() returned NULL!\n");
    return -1;
  }
  size_t newsize = strlen(wisdom);
  if(newsize <= Wisdom_size || (Wisdom_string != NULL && strncmp(wisdom,Wisdom_string,newsize) == 0)){
    // no change
    FREE(wisdom);
    return 0;
  }
  // Write new wisdom to temp file
  char *newtemp = NULL;
  if(asprintf(&newtemp,"%s-XXXXXX",Arch_wisdom_file) < 0){
    printf("Can't create temporary wisdom file name: %s\n",strerror(errno));
    FREE(newtemp);
    FREE(wisdom);
    return -1;
  }
  int fd = mkstemp(newtemp);
  if(fd == -1){
    printf("Can't create temporary wisdom file %s: %s\n",newtemp,strerror(errno));
    FREE(newtemp);
    FREE(wisdom);
    return -1;
  }
  fchmod(fd,0664); // I really do want rw-rw-r-- so the radio group can write it
  char *lockfile = NULL;
  if(asprintf(&lockfile,"%s.lock",wisdom_file) <= 0){
    close(fd);
    FREE(wisdom);
    FREE(lockfile);
    FREE(newtemp);
    return -1;
  }
  int lockfd = open(lockfile,O_CREAT|O_RDWR,0664);
  fchmod(lockfd,0664); // I really do want rw-rw-r-- so the radio group can write it
  if(lockfd == -1){
    printf("Can't acquire lock on %s\n",lockfile);
    close(fd);
    FREE(wisdom);
    FREE(lockfile);
    FREE(newtemp);
    return -1;
  }
  flock(lockfd,LOCK_EX);
  FREE(Wisdom_string);
  Wisdom_string = wisdom;
  Wisdom_size = newsize;
  if(write(fd,Wisdom_string, Wisdom_size) != (ssize_t)Wisdom_size){
    printf("Write of new wisdom file length %lu failed: %s\n",(unsigned long)newsize,strerror(errno));
    close(fd);
    FREE(newtemp);
    unlink(lockfile);
    close(lockfd);
    FREE(lockfile);
    return -1;
  }
  {
    struct stat st = {0};
    if(fstat(fd,&st) != 0 || st.st_size <= 0){
      printf("stat of new wisdom file %s failed: %s\n",newtemp,strerror(errno));
      close(fd);
      FREE(newtemp);
      unlink(lockfile);
      close(lockfd);
      FREE(lockfile);
      return -1;
    }
  }
  // Copy user/group/mode from old version
  {
    struct stat st = {0};
    if(stat(Arch_wisdom_file,&st) != 0)
      printf("stat(%s) failed: %s\n",Arch_wisdom_file,strerror(errno));
    else {
      int r = fchown(fd, st.st_uid, st.st_gid); // Best effort
      if(r != 0)
	printf("fchown(%s) failed: %s\n",newtemp,strerror(errno));
      r = fchmod(fd, st.st_mode);
      if(r != 0)
	printf("fchmod(%s) failed: %s\n",newtemp,strerror(errno));
    }
  }
  // Make sure it's really, really out there
  fsync(fd);
  close(fd);
  fd = -1;
  {
    int dir = open(STATEDIR,O_DIRECTORY);
    fsync(dir);
    close(dir);
  }
  if(rename(newtemp,Arch_wisdom_file) != 0){
    printf("rename %s to %s failed: %s\n",newtemp,Arch_wisdom_file,strerror(errno));
    FREE(newtemp);
    unlink(lockfile);
    close(lockfd);
    FREE(lockfile);
    return -1;
  }
  if(Verbose > 1)
    printf("rename %s to %s succeeded\n",newtemp,Arch_wisdom_file);

  if(Force)
    fftwf_forget_wisdom(); // start fresh for the next on the list

  FREE(newtemp);
  unlink(lockfile);
  close(lockfd);
  FREE(lockfile);
  return 0;
}
static void load_wisdom(void){
  bool r = fftwf_import_system_wisdom();
  if(Verbose > 1){
    printf("fftwf_import_system_wisdom() %s\n",r ? "succeeded" : "failed");
    if(!r && access(SYSTEM_WISDOM_FILE,R_OK) == -1) // Would really like to use AT_EACCESS flag
      printf("system wisdom %s not readable: %s\n",SYSTEM_WISDOM_FILE,strerror(errno));
  }
  r = fftwf_import_wisdom_from_filename(GENERIC_WISDOM_FILE);
  if(Verbose > 1){
    printf("fftwf_import_wisdom_from_filename(%s) %s\n",GENERIC_WISDOM_FILE,r ? "succeeded" : "failed");
    if(!r && access(GENERIC_WISDOM_FILE,R_OK) == -1)
      printf("%s not readable: %s\n",GENERIC_WISDOM_FILE,strerror(errno));
  }
  r = fftwf_import_wisdom_from_filename(Arch_wisdom_file);
  if(Verbose > 1){
    printf("fftwf_import_wisdom_from_filename(%s) %s\n",Arch_wisdom_file,r ? "succeeded" : "failed");
    if(!r && access(Arch_wisdom_file,R_OK) == -1)
      printf("%s not readable: %s\n",Arch_wisdom_file,strerror(errno));
  }
}

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

static void usage(void){
  printf("fft-gen: creates and updates ffw3f wisdom for ka9q-radio\n");
  printf("usage: fft-gen [-h] [-v|--verbose [-v|--verbose]] [--timelimit|-t sec] [--threads|-T <n>] [--force|-f] [--patient|--measure|--estimate|--exhaustive|-x|-e|-m|-p] transform...\n");
  printf("  eg   fft-gen -v --exhaustive cob200 cob300 cob400 cob600 cob1200\n");
}
