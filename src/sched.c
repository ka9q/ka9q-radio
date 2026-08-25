// Scheduler-related routines
// enable/disable realtime scheduling, cpu affinity (Linux only)
// copyright 2026 Phil Karn KA9Q
#include <sched.h>
#include <stdatomic.h>
#include <pthread.h>
#include <sys/resource.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <errno.h>
#include <limits.h>
#include <ctype.h>
#include <stdlib.h>
#include <unistd.h>
#include "sched.h"

// Set realtime priority (if possible)
static int Base_prio;
#if defined(__linux__)
static atomic_flag Sched_message_shown = ATOMIC_FLAG_INIT;
#endif
static atomic_flag Nice_message_shown = ATOMIC_FLAG_INIT;

// Run at real time priority, if possible
// On Linux this uses the real time scheduler
// If that doesn't work (possibly due to lack of permissions) or isn't available, try nice (which also requires permissions)
void realtime(int prio){
  if(prio == 0)
    return;

#if defined(__linux__)
  struct sched_param param = {0};

  param.sched_priority = prio;
  if(sched_setscheduler(0,SCHED_FIFO,&param) == 0)
    return; // Successfully set realtime
  {
    char name[25];
    int err = errno;
    if(pthread_getname_np(pthread_self(),name,sizeof(name)) == 0){
      if(!atomic_flag_test_and_set_explicit(&Sched_message_shown,memory_order_relaxed))
	fprintf(stderr,"%s: sched_setscheduler failed, %s (%d)\n",name,strerror(err),err);
    }
  }
#endif
  // As backup, decrease our niceness by 10
  Base_prio = getpriority(PRIO_PROCESS,0);
  errno = 0; // setpriority can return -1
  int niceness = setpriority(PRIO_PROCESS,0,Base_prio - 10);
  if(niceness != 0){
    int err = errno;
    char name[25];
    memset(name,0,sizeof(name));
    if(pthread_getname_np(pthread_self(),name,sizeof(name)-1) == 0){
      if(!atomic_flag_test_and_set_explicit(&Nice_message_shown,memory_order_relaxed))
	fprintf(stderr,"%s: setpriority failed, %s (%d)\n",name,strerror(err),err);
    }
  }
}

// Drop back to normal priority, to avoid blocking a core when doing something time-consuming, like a FFT plan
// Return true if we had previously been running at realtime, false otherwise
int norealtime(void){
#ifdef __linux__
  if(sched_getscheduler(0) == SCHED_OTHER)
    return 0; // Already normal

  struct sched_param param = {0};
  sched_getparam(0,&param);
  int old_priority = param.sched_priority;

  param.sched_priority = 0;
  if(sched_setscheduler(0,SCHED_OTHER,&param) == 0)
    return old_priority; // Successfully returned to normal

  {
    int err = errno; // in case getname changes it
    char name[25] = {0};
    if(pthread_getname_np(pthread_self(),name,sizeof(name)) == 0){
      if(!atomic_flag_test_and_set_explicit(&Sched_message_shown,memory_order_relaxed))
	fprintf(stderr,"%s: sched_setscheduler failed, %s (%d)\n",name,strerror(err),err);
    }
  }
#endif
  // Try renicing to our base prio
  errno = 0; // setpriority can return -1
  int prio = getpriority(PRIO_PROCESS,0);
  if(prio == Base_prio)
    return prio; // Already normal niceness

  prio = setpriority(PRIO_PROCESS,0,Base_prio);
  if(prio == 0)
    return prio; // Successfully returned to normal niceness
  // Can it really fail when we're lowering?
  int err = errno;
  char name[25] = {0};
  if(pthread_getname_np(pthread_self(),name,sizeof(name)-1) == 0){
    if(!atomic_flag_test_and_set_explicit(&Nice_message_shown,memory_order_relaxed))
      fprintf(stderr,"%s: setpriority failed to lower, %s (%d)\n",name,strerror(err),err);
  }
  return prio;  // Don't really know our state
}

bool Affinity = false;

#ifndef __linux__
void stick_core(void){}
#else // Affinity is Linux only, Affinity setting is ignored otherwise
void stick_core(void){
  if(!Affinity)
    return;

  // Stay on this CPU core
  char name[25] = {0};
  pthread_t self = pthread_self();
  if(pthread_getname_np(self,name,sizeof(name)-1) != 0)
    fprintf(stderr,"getname(%ud) failed: %s\n",(unsigned int)self,strerror(errno));

  int cpu = sched_getcpu();
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(cpu,&cpuset);
  flockfile(stderr); // don't let this line get broken up
  fprintf(stderr,"%s sched_setaffinity(pid=%u,cores=",name,(unsigned int)self);
  // Any hyperthreading siblings?
  char sysname[PATH_MAX] = {0};
  snprintf(sysname,sizeof sysname,"/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list",cpu);
  FILE *fp = fopen(sysname,"r");
  if(fp != NULL){
    char corelist[128] = {0};
    if(fgets(corelist,sizeof corelist,fp) != NULL){
      // thread_siblings_list may be comma-separated ("0,1") or a range
      // ("0-1"); parse both so SMT siblings aren't dropped (strtol stops at '-').
      char *string = corelist;
      char const *ptr = NULL;
      while((ptr = strsep(&string,",")) != NULL){
	char const *dash = strchr(ptr,'-');
	int lo = (int)strtol(ptr,NULL,0);
	int hi = dash ? (int)strtol(dash+1,NULL,0) : lo;
	for(int c = lo; c <= hi && c < CPU_SETSIZE; c++){
	  CPU_SET(c,&cpuset);
	  fprintf(stderr," %d",c);
	}
      }
    }
    fclose(fp);
    fp = NULL;
  } else {
    fprintf(stderr," %d",cpu);
  }
  fprintf(stderr,")");
  if (sched_setaffinity(0, sizeof(cpuset), &cpuset) == -1) {
    fprintf(stderr," failed: %s\n",strerror(errno));
  } else {
    fprintf(stderr,"\n");
  }
  funlockfile(stderr);
}
// Extra stuff written by ChatGPT that might prove useful someday
#if 0
#include <dirent.h>

#ifndef CPU_SETSIZE
#define CPU_SETSIZE 1024
#endif

static bool parse_cpu_list(const char *s, cpu_set_t *out) {
  // Parses Linux cpu list syntax: "0", "0,2,4-7", "1-3,8,10-11"
  CPU_ZERO(out);
  const char *p = s;

  while (*p) {
    while (isspace((unsigned char)*p)) p++;
    if (!*p) break;

    if (!isdigit((unsigned char)*p)) return false;

    char *end = NULL;
    long a = strtol(p, &end, 10);
    if (end == p || a < 0 || a >= CPU_SETSIZE) return false;
    p = end;

    long b = a;
    if (*p == '-') {
      p++;
      if (!isdigit((unsigned char)*p)) return false;
      long v = strtol(p, &end, 10);
      if (end == p || v < 0 || v >= CPU_SETSIZE) return false;
      b = v;
      p = end;
    }

    if (a > b) {
      long tmp = a; a = b; b = tmp;
    }
    for (long i = a; i <= b; i++) CPU_SET((int)i, out);

    while (isspace((unsigned char)*p)) p++;
    if (*p == ',') {
      p++;
      continue;
    }
    if (*p == '\0' || *p == '\n') break;
    // Unexpected character
    return false;
  }
  return true;
}

static int read_file_to_buf(const char *path, char *buf, size_t buflen) {
  FILE *f = fopen(path, "r");
  if (!f) return -1;
  if (!fgets(buf, (int)buflen, f)) {
    fclose(f);
    errno = EIO;
    return -1;
  }
  fclose(f);
  // strip trailing newline
  buf[strcspn(buf, "\r\n")] = 0;
  return 0;
}

// Enumerate online CPUs by scanning /sys/devices/system/cpu/cpuN directories.
static int enumerate_online_cpus(int *cpus, int max_cpus) {
  DIR *d = opendir("/sys/devices/system/cpu");
  if (!d) return -1;

  int n = 0;
  struct dirent *de;
  while ((de = readdir(d)) != NULL) {
    if (strncmp(de->d_name, "cpu", 3) != 0) continue;
    const char *p = de->d_name + 3;
    if (!isdigit((unsigned char)*p)) continue;

    char *end = NULL;
    long cpu = strtol(p, &end, 10);
    if (end == p || *end != '\0') continue;
    if (cpu < 0 || cpu >= CPU_SETSIZE) continue;

    // Check online status (cpu0 may not have "online"; treat as online).
    char online_path[PATH_MAX];
    snprintf(online_path, sizeof online_path,
             "/sys/devices/system/cpu/cpu%ld/online", cpu);

    char buf[32];
    bool online = true;
    if (access(online_path, R_OK) == 0) {
      if (read_file_to_buf(online_path, buf, sizeof buf) == 0) {
        online = (buf[0] == '1');
      }
    }
    if (!online) continue;

    if (n < max_cpus) cpus[n] = (int)cpu;
    n++;
  }
  closedir(d);
  return n; // may be > max_cpus
}

// Read SMT siblings for a given CPU into a cpu_set_t.
static int get_thread_siblings(int cpu, cpu_set_t *siblings_out) {
  char path[PATH_MAX];
  snprintf(path, sizeof path,
           "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list", cpu);

  char buf[256];
  if (read_file_to_buf(path, buf, sizeof buf) < 0) return -1;
  if (!parse_cpu_list(buf, siblings_out)) {
    errno = EINVAL;
    return -1;
  }
  return 0;
}
// Helpers
static int cpu_first(const cpu_set_t *s) {
  for (int i = 0; i < CPU_SETSIZE; i++)
    if (CPU_ISSET(i, s)) return i;
  return -1;
}

static int cpu_count(const cpu_set_t *s) {
  int n = 0;
  for (int i = 0; i < CPU_SETSIZE; i++)
    if (CPU_ISSET(i, s)) n++;
  return n;
}

static void cpu_intersect(cpu_set_t *dst, const cpu_set_t *a, const cpu_set_t *b) {
  CPU_ZERO(dst);
  for (int i = 0; i < CPU_SETSIZE; i++)
    if (CPU_ISSET(i, a) && CPU_ISSET(i, b)) CPU_SET(i, dst);
}

static bool cpu_equal(const cpu_set_t *a, const cpu_set_t *b) {
  for (int i = 0; i < CPU_SETSIZE; i++)
    if (CPU_ISSET(i, a) != CPU_ISSET(i, b)) return false;
  return true;
}


typedef struct {
  cpu_set_t cpus;   // allowed logical CPUs for this physical core (often 2 CPUs)
  int rep_cpu;      // representative CPU (lowest) inside cpus
} core_group_t;

typedef struct {
  core_group_t *v;
  size_t n, cap;
} core_group_vec_t;

static void vec_free(core_group_vec_t *cv) {
  free(cv->v);
  cv->v = NULL; cv->n = cv->cap = 0;
}

static int vec_push(core_group_vec_t *cv, const core_group_t *g) {
  if (cv->n == cv->cap) {
    size_t newcap = cv->cap ? cv->cap * 2 : 16;
    void *p = realloc(cv->v, newcap * sizeof(core_group_t));
    if (!p) return -1;
    cv->v = (core_group_t*)p;
    cv->cap = newcap;
  }
  cv->v[cv->n++] = *g;
  return 0;
}

// Example: build an array mapping each cpu -> its sibling mask.
static int build_sibling_map(cpu_set_t *map, int map_len) {
  int cpus[CPU_SETSIZE];
  int n = enumerate_online_cpus(cpus, CPU_SETSIZE);
  if (n < 0) return -1;

  for (int i = 0; i < n && i < CPU_SETSIZE; i++) {
    int cpu = cpus[i];
    if (cpu < map_len) {
      if (get_thread_siblings(cpu, &map[cpu]) < 0) return -1;
    }
  }
  return 0;
}

// Build physical-core groups constrained to the process' allowed cpuset.
// Each group is (thread_siblings_list(cpu) ∩ allowed).
static int build_core_groups(core_group_vec_t *out_groups, cpu_set_t *out_allowed) {
  memset(out_groups, 0, sizeof(*out_groups));
  CPU_ZERO(out_allowed);

  // Allowed CPUs from systemd/cpuset/CPUAffinity
  if (sched_getaffinity(0, sizeof(cpu_set_t), out_allowed) != 0)
    return -1;

  // Walk allowed CPUs; for each, form its sibling-set intersect allowed; dedupe.
  cpu_set_t seen_groups_mask[1]; // not used; we dedupe by comparing cpu_set_t

  (void)seen_groups_mask;

  for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
    if (!CPU_ISSET(cpu, out_allowed)) continue;

    cpu_set_t sib;
    if (get_thread_siblings(cpu, &sib) != 0) {
      // If sysfs missing (rare), degrade gracefully: treat as singleton.
      CPU_ZERO(&sib);
      CPU_SET(cpu, &sib);
    }

    cpu_set_t gset;
    cpu_intersect(&gset, &sib, out_allowed);
    if (cpu_count(&gset) == 0) continue;

    // Dedupe: don't add if an identical group already exists.
    bool exists = false;
    for (size_t i = 0; i < out_groups->n; i++) {
      if (cpu_equal(&out_groups->v[i].cpus, &gset)) { exists = true; break; }
    }
    if (exists) continue;

    core_group_t g;
    g.cpus = gset;
    g.rep_cpu = cpu_first(&gset);
    if (g.rep_cpu < 0) continue;

    if (vec_push(out_groups, &g) != 0) {
      vec_free(out_groups);
      return -1;
    }
  }
  return 0;
}

// Set affinity for current thread.
static int set_this_thread_affinity(const cpu_set_t *mask) {
  return pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), mask);
}
static ssize_t pick_fft_group(const core_group_vec_t *g) {
  if (g->n == 0) return -1;
  size_t best = 0;
  for (size_t i = 1; i < g->n; i++) {
    if (g->v[i].rep_cpu > g->v[best].rep_cpu)
      best = i;
  }
  return (ssize_t)best;
}
static size_t map_channel_to_group(size_t chan_id, size_t n_groups, size_t fft_index) {
  // Map into groups excluding fft_index.
  size_t usable = (n_groups > 0) ? (n_groups - 1) : 0;
  if (usable == 0) return fft_index; // degenerate
  size_t k = chan_id % usable;
  // Convert k into an index in [0..n_groups) skipping fft_index
  if (k >= (size_t)fft_index) k++;
  return k;
}
#endif
#endif
/* sample usage
   cpu_set_t allowed;
   core_group_vec_t groups;
   if (build_core_groups(&groups, &allowed) != 0) { handle }

   ssize_t fft_i = pick_fft_group(&groups);
   if (fft_i >= 0) {
   // Pin FFT thread to BOTH SMT siblings of its core
   set_this_thread_affinity(&groups.v[fft_i].cpus);
   }

   size_t gi = map_channel_to_group(channel_id, groups.n, (size_t)fft_i);

   // Option 1: allow SMT siblings (recommended if threads are short)
   set_this_thread_affinity(&groups.v[gi].cpus);

   // Option 2: avoid SMT, pick only rep_cpu
   cpu_set_t one; CPU_ZERO(&one); CPU_SET(groups.v[gi].rep_cpu, &one);
   set_this_thread_affinity(&one);
*/
