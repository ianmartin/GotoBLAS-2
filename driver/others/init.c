/*********************************************************************/
/* Copyright 2009, 2010 The University of Texas at Austin.           */
/* All rights reserved.                                              */
/*                                                                   */
/* Redistribution and use in source and binary forms, with or        */
/* without modification, are permitted provided that the following   */
/* conditions are met:                                               */
/*                                                                   */
/*   1. Redistributions of source code must retain the above         */
/*      copyright notice, this list of conditions and the following  */
/*      disclaimer.                                                  */
/*                                                                   */
/*   2. Redistributions in binary form must reproduce the above      */
/*      copyright notice, this list of conditions and the following  */
/*      disclaimer in the documentation and/or other materials       */
/*      provided with the distribution.                              */
/*                                                                   */
/*    THIS  SOFTWARE IS PROVIDED  BY THE  UNIVERSITY OF  TEXAS AT    */
/*    AUSTIN  ``AS IS''  AND ANY  EXPRESS OR  IMPLIED WARRANTIES,    */
/*    INCLUDING, BUT  NOT LIMITED  TO, THE IMPLIED  WARRANTIES OF    */
/*    MERCHANTABILITY  AND FITNESS FOR  A PARTICULAR  PURPOSE ARE    */
/*    DISCLAIMED.  IN  NO EVENT SHALL THE UNIVERSITY  OF TEXAS AT    */
/*    AUSTIN OR CONTRIBUTORS BE  LIABLE FOR ANY DIRECT, INDIRECT,    */
/*    INCIDENTAL,  SPECIAL, EXEMPLARY,  OR  CONSEQUENTIAL DAMAGES    */
/*    (INCLUDING, BUT  NOT LIMITED TO,  PROCUREMENT OF SUBSTITUTE    */
/*    GOODS  OR  SERVICES; LOSS  OF  USE,  DATA,  OR PROFITS;  OR    */
/*    BUSINESS INTERRUPTION) HOWEVER CAUSED  AND ON ANY THEORY OF    */
/*    LIABILITY, WHETHER  IN CONTRACT, STRICT  LIABILITY, OR TORT    */
/*    (INCLUDING NEGLIGENCE OR OTHERWISE)  ARISING IN ANY WAY OUT    */
/*    OF  THE  USE OF  THIS  SOFTWARE,  EVEN  IF ADVISED  OF  THE    */
/*    POSSIBILITY OF SUCH DAMAGE.                                    */
/*                                                                   */
/* The views and conclusions contained in the software and           */
/* documentation are those of the authors and should not be          */
/* interpreted as representing official policies, either expressed   */
/* or implied, of The University of Texas at Austin.                 */
/*********************************************************************/

#include "common.h"

#if defined(OS_LINUX) && defined(SMP)

#define _GNU_SOURCE

#include <sys/sysinfo.h>
#include <sys/syscall.h>
#include <sys/shm.h>
#include <fcntl.h>
#include <sched.h>
#include <dirent.h>
#include <dlfcn.h>

#define MAX_NODES	16
#define MAX_CPUS	256

#define SH_MAGIC	0x510510

#define CPUMAP_NAME	"/sys/devices/system/node/node%d/cpumap"
#define SHARE_NAME	"/sys/devices/system/cpu/cpu%d/cache/index%d/shared_cpu_map"
#define NODE_DIR	"/sys/devices/system/node"

#undef DEBUG

/* Private variables */
typedef struct {
  unsigned long lock;
  unsigned int magic;
  unsigned int shmid;

  int num_nodes;
  int num_procs;
  int final_num_procs;
  unsigned long avail;
  
  unsigned long cpu_info   [MAX_CPUS];
  unsigned long node_info  [MAX_NODES];
  int cpu_use[MAX_CPUS];

} shm_t;

static cpu_set_t cpu_orig_mask[4];

static int  cpu_mapping[MAX_CPUS];
static int  node_mapping[MAX_CPUS * 4];
static int  cpu_sub_mapping[MAX_CPUS];
static int  disable_mapping;

/* Number of cores per nodes */
static int  node_cpu[MAX_NODES];
static int  node_equal = 0;

static shm_t *common = (void *)-1;
static int shmid, pshmid;
static void *paddr;

static unsigned long lprocmask, lnodemask;
static int numprocs = 1;
static int numnodes = 1;

#if 1
#define READ_CPU(x)   ( (x)        & 0xff)
#define READ_NODE(x)  (((x) >>  8) & 0xff)
#define READ_CORE(x)  (((x) >> 16) & 0xff)

#define WRITE_CPU(x)    (x)
#define WRITE_NODE(x)  ((x) <<  8)
#define WRITE_CORE(x)  ((x) << 16)
#else
#define READ_CPU(x)   ( (x)        & 0xff)
#define READ_CORE(x)  (((x) >>  8) & 0xff)
#define READ_NODE(x)  (((x) >> 16) & 0xff)

#define WRITE_CPU(x)    (x)
#define WRITE_CORE(x)  ((x) <<  8)
#define WRITE_NODE(x)  ((x) << 16)
#endif

static inline int popcount(unsigned long number) {

  int count = 0;

  while (number > 0) {
    if (number & 1) count ++;
    number >>= 1;
  }

  return count;
}

static inline int rcount(unsigned long number) {

  int count = -1;

  while ((number > 0) && ((number & 0)) == 0) {
    count ++;
    number >>= 1;
  }

  return count;
}

static inline unsigned long get_cpumap(int node) {

  int infile;
  unsigned long affinity;
  char name[160];
  char *p, *dummy;
  
  sprintf(name, CPUMAP_NAME, node);
  
  infile = open(name, O_RDONLY);

  affinity = 0;
    
  if (infile != -1) {
    
    read(infile, name, sizeof(name));
   
    p = name;

    while ((*p == '0') || (*p == ',')) p++;

    affinity = strtol(p, &dummy, 16);
   
    close(infile);
  }

  return affinity;
}

static inline unsigned long get_share(int cpu, int level) {

  int infile;
  unsigned long affinity;
  char name[160];
  char *p;
  
  sprintf(name, SHARE_NAME, cpu, level);
  
  infile = open(name, O_RDONLY);

  affinity = (1UL << cpu);
    
  if (infile != -1) {
    
    read(infile, name, sizeof(name));
   
    p = name;

    while ((*p == '0') || (*p == ',')) p++;

    affinity = strtol(p, &p, 16);
   
    close(infile);
  }

  return affinity;
}

static int numa_check(void) {

  DIR *dp;
  struct dirent *dir;
  int node;

  common -> num_nodes = 0;

  dp = opendir(NODE_DIR);

  if (dp == NULL) {
    common -> num_nodes = 1;
    return 0;
  }

  for (node = 0; node < MAX_NODES; node ++) common -> node_info[node] = 0;

  while ((dir = readdir(dp)) != NULL) {
    if (*(unsigned int *) dir -> d_name == 0x065646f6eU) {

      node = atoi(&dir -> d_name[4]);

      if (node > MAX_NODES) {
	fprintf(stderr, "\nGotoBLAS Warining : MAX_NODES (NUMA) is too small. Terminated.\n");
	exit(1);
      }

      common -> num_nodes ++;
      common -> node_info[node] = get_cpumap(node);

    }
  }

  closedir(dp);

  if (common -> num_nodes == 1) return 1;

#ifdef DEBUG
  fprintf(stderr, "Numa found : number of Nodes = %2d\n", common -> num_nodes);

  for (node = 0; node < common -> num_nodes; node ++)
    fprintf(stderr, "MASK (%2d) : %08lx\n", node, common -> node_info[node]);
#endif

  return common -> num_nodes;
}

static void numa_mapping(void) {

  int node, cpu, core;
  int i, j, h;
  unsigned long work, bit;
  int count = 0;

  for (node = 0; node < common -> num_nodes; node ++) {
    core = 0;
    for (cpu = 0; cpu < common -> num_procs; cpu ++) {
      if (common -> node_info[node] & common -> avail & (1UL << cpu)) {
	common -> cpu_info[count] = WRITE_CORE(core) | WRITE_NODE(node) | WRITE_CPU(cpu);
	count ++;
	core ++;
      }

    }
  }

#ifdef DEBUG
  fprintf(stderr, "\nFrom /sys ...\n\n");

  for (cpu = 0; cpu < count; cpu++) 
    fprintf(stderr, "CPU (%2d) : %08lx\n", cpu, common -> cpu_info[cpu]);
#endif

  h = 1;

  while (h < count) h = 2 * h + 1;

  while (h > 1) {
    h /= 2;
    for (i = h; i < count; i++) {
      work = common -> cpu_info[i];
      bit  = CPU_ISSET(i, &cpu_orig_mask[0]);
      j = i - h;
      while (work < common -> cpu_info[j]) {
	common -> cpu_info[j + h] = common -> cpu_info[j];
	if (CPU_ISSET(j, &cpu_orig_mask[0])) {
	  CPU_SET(j + h, &cpu_orig_mask[0]);
	} else {
	  CPU_CLR(j + h, &cpu_orig_mask[0]);
	}
	j -= h;
	if (j < 0) break;
      }
      common -> cpu_info[j + h] = work;
      if (bit) {
	CPU_SET(j + h, &cpu_orig_mask[0]);
      } else {
	CPU_CLR(j + h, &cpu_orig_mask[0]);
      }

    }
  }

#ifdef DEBUG
  fprintf(stderr, "\nSorting ...\n\n");

  for (cpu = 0; cpu < count; cpu++) 
    fprintf(stderr, "CPU (%2d) : %08lx\n", cpu, common -> cpu_info[cpu]);
#endif

}

static void disable_hyperthread(void) {

  unsigned long share;
  int cpu;

  common -> avail = (1UL << common -> num_procs) - 1;

#ifdef DEBUG
  fprintf(stderr, "\nAvail CPUs    : %04lx.\n", common -> avail);
#endif

  for (cpu = 0; cpu < common -> num_procs; cpu ++) {
    
    share = (get_share(cpu, 1) & common -> avail);
    
    if (popcount(share) > 1) {
      
#ifdef DEBUG
      fprintf(stderr, "Detected Hyper Threading on CPU %4x; disabled CPU %04lx.\n",
	      cpu, share & ~(1UL << cpu));
#endif
      
      common -> avail &= ~((share & ~(1UL << cpu)));
    }
  }
}

static void disable_affinity(void) {

#ifdef DEBUG
    fprintf(stderr, "Final all available CPUs  : %04lx.\n\n", common -> avail);
    fprintf(stderr, "CPU mask                  : %04lx.\n\n", *(unsigned long *)&cpu_orig_mask[0]);
#endif

  lprocmask = (1UL << common -> final_num_procs) - 1;

#ifndef USE_OPENMP
  lprocmask &= *(unsigned long *)&cpu_orig_mask[0];
#endif

#ifdef DEBUG
    fprintf(stderr, "I choose these CPUs  : %04lx.\n\n", lprocmask);
#endif

}

static void setup_mempolicy(void) {

  int cpu, mynode, maxcpu;

  for (cpu = 0; cpu < MAX_NODES; cpu ++) node_cpu[cpu] = 0;

  maxcpu = 0;

  for (cpu = 0; cpu < numprocs; cpu ++) {
    mynode = READ_NODE(common -> cpu_info[cpu_sub_mapping[cpu]]);
    
    lnodemask |= (1UL << mynode);

    node_cpu[mynode] ++;

    if (maxcpu < node_cpu[mynode]) maxcpu = node_cpu[mynode];
  }

  node_equal = 1;

  for (cpu = 0; cpu < MAX_NODES; cpu ++) if ((node_cpu[cpu] != 0) && (node_cpu[cpu] != maxcpu)) node_equal = 0;

  if (lnodemask) {
  
#ifdef DEBUG
    fprintf(stderr, "Node mask = %lx\n", lnodemask);
#endif
    
    my_set_mempolicy(MPOL_INTERLEAVE, &lnodemask, sizeof(lnodemask) * 8);

    numnodes = popcount(lnodemask);
  }
}

static inline int is_dead(int id) {

  struct shmid_ds ds;

  return shmctl(id, IPC_STAT, &ds);
}
static void open_shmem(void) {

  int try = 0;

  do {

    shmid = shmget(SH_MAGIC, 4096, 0666);
    
    if (shmid == -1) {
      shmid = shmget(SH_MAGIC, 4096, IPC_CREAT | 0666);
    }
    
    try ++;

  } while ((try < 10) && (shmid == -1));

  if (shmid == -1) {
    fprintf(stderr, "GotoBLAS : Can't open shared memory. Terminated.\n");
    exit(1);
  }

  if (shmid != -1) common = (shm_t *)shmat(shmid, NULL, 0);

#ifdef DEBUG
  fprintf(stderr, "Shared Memory id = %x  Address = %p\n", shmid, common);
#endif

}

static void create_pshmem(void) {

  pshmid = shmget(IPC_PRIVATE, 4096, IPC_CREAT | 0666);

  paddr = shmat(pshmid, NULL, 0);

  shmctl(pshmid, IPC_RMID, 0);

#ifdef DEBUG
  fprintf(stderr, "Private Shared Memory id = %x  Address = %p\n", pshmid, paddr);
#endif
}

static void local_cpu_map(void) {

  int cpu, id, mapping;

  cpu = 0;
  mapping = 0;

  do {
    id   = common -> cpu_use[cpu];

    if (id > 0) {
      if (is_dead(id)) common -> cpu_use[cpu] = 0;
    }

    if ((common -> cpu_use[cpu] == 0) && (lprocmask & (1UL << cpu))) {

      common -> cpu_use[cpu] = pshmid;
      cpu_mapping[mapping] = READ_CPU(common -> cpu_info[cpu]);
      cpu_sub_mapping[mapping] = cpu;

      mapping ++;
    }

    cpu ++;
    
  } while ((mapping < numprocs) && (cpu < common -> final_num_procs));
  
  disable_mapping = 0;

  if ((mapping < numprocs) || (numprocs == 1)) {
    for (cpu = 0; cpu < common -> final_num_procs; cpu ++) {
      if (common -> cpu_use[cpu] == pshmid) common -> cpu_use[cpu] = 0;
    }
    disable_mapping = 1;
  }
  
#ifdef DEBUG
  for (cpu = 0; cpu < numprocs; cpu ++) {
    fprintf(stderr, "Local Mapping  : %2d --> %2d (%2d)\n", cpu, cpu_mapping[cpu], cpu_sub_mapping[cpu]);
  }
#endif
}

/* Public Functions */

int get_num_procs(void)  { return numprocs; }
int get_num_nodes(void)  { return numnodes; }
int get_node_equal(void) { 

  return (((blas_cpu_number % numnodes) == 0) && node_equal);
  
}

int gotoblas_set_affinity(int pos) {
  
  cpu_set_t cpu_mask;

  int mynode = 1;

  /* if number of threads is larger than inital condition */
  if (pos < 0) {
      sched_setaffinity(0, sizeof(cpu_orig_mask), &cpu_orig_mask[0]);
      return 0;
  }

  if (!disable_mapping) {

    mynode = READ_NODE(common -> cpu_info[cpu_sub_mapping[pos]]);

#ifdef DEBUG
    fprintf(stderr, "Giving Affinity[%4d   %3d] --> %3d  My node = %3d\n", getpid(), pos, cpu_mapping[pos], mynode);
#endif

    CPU_ZERO(&cpu_mask);
    CPU_SET (cpu_mapping[pos], &cpu_mask);
    
    sched_setaffinity(0, sizeof(cpu_mask), &cpu_mask);

    node_mapping[WhereAmI()] = mynode;

  }

  return mynode;
}

int get_node(void) { 

  if (!disable_mapping) return node_mapping[WhereAmI()];

  return 1;
}

static int initialized = 0;

void gotoblas_affinity_init(void) {

  int cpu, num_avail;
#ifndef USE_OPENMP
  cpu_set_t cpu_mask;
#endif

  if (initialized) return;

  initialized = 1;

  sched_getaffinity(0, sizeof(cpu_orig_mask), &cpu_orig_mask[0]);
    
#ifdef USE_OPENMP
  numprocs = 0;
#else
  numprocs = readenv("GOTO_NUM_THREADS");
#endif

  if (numprocs == 0) numprocs = readenv("OMP_NUM_THREADS");

  numnodes = 1;

  if (numprocs == 1) {
    disable_mapping = 1;
    return;
  }

  create_pshmem();

  open_shmem();

  while ((common -> lock) && (common -> magic != SH_MAGIC)) {
    if (is_dead(common -> shmid)) {
      common -> lock = 0;
      common -> shmid = 0;
      common -> magic = 0;
    } else {
      sched_yield();
    }
  }

  blas_lock(&common -> lock);

  if ((common -> shmid) && is_dead(common -> shmid)) common -> magic = 0;

  common -> shmid = pshmid;

  if (common -> magic != SH_MAGIC) {

#ifdef DEBUG
    fprintf(stderr, "Shared Memory Initialization.\n");
#endif

    common -> num_procs = get_nprocs();

    for (cpu = 0; cpu < common -> num_procs; cpu++) common -> cpu_info[cpu] = cpu;
    
    numa_check();
    
    disable_hyperthread();

    if (common -> num_nodes > 1) numa_mapping();

    common -> final_num_procs = popcount(common -> avail);

    for (cpu = 0; cpu < common -> final_num_procs; cpu ++) common -> cpu_use[cpu] =  0;

    common -> magic = SH_MAGIC;

  }

  disable_affinity();

  num_avail = popcount(lprocmask);

  if ((numprocs <= 0) || (numprocs > num_avail)) numprocs = num_avail;

#ifdef DEBUG
  fprintf(stderr, "Number of threads = %d\n", numprocs);
#endif

  local_cpu_map();

  blas_unlock(&common -> lock);

#ifndef USE_OPENMP
  if (!disable_mapping) {

#ifdef DEBUG
    fprintf(stderr, "Giving Affinity[%3d] --> %3d\n", 0, cpu_mapping[0]);
#endif

    CPU_ZERO(&cpu_mask);
    CPU_SET (cpu_mapping[0], &cpu_mask);
    
    sched_setaffinity(0, sizeof(cpu_mask), &cpu_mask);

    node_mapping[WhereAmI()] = READ_NODE(common -> cpu_info[cpu_sub_mapping[0]]);

    setup_mempolicy();

    if (readenv("GOTOBLAS_MAIN_FREE")) {
      sched_setaffinity(0, sizeof(cpu_orig_mask), &cpu_orig_mask[0]);
    }

  }
#endif

#ifdef DEBUG
  fprintf(stderr, "Initialization is done.\n");
#endif
}

void gotoblas_affinity_quit(void) {

  int i;
  struct shmid_ds ds;

#ifdef DEBUG
  fprintf(stderr, "Terminating ..\n");
#endif

  if ((numprocs == 1) || (initialized == 0)) return;

  if (!disable_mapping) {
    
    blas_lock(&common -> lock);
    
    for (i = 0; i < numprocs; i ++) common -> cpu_use[cpu_mapping[i]] = -1;
    
    blas_unlock(&common -> lock);
  
  }

  shmctl(shmid, IPC_STAT, &ds);

  if (ds.shm_nattch == 1) shmctl(shmid, IPC_RMID, 0);

  shmdt(common);

  shmdt(paddr);

  initialized = 0;
}

#else

void gotoblas_affinity_init(void) {};

void gotoblas_set_affinity(int threads) {};

void gotoblas_set_affinity2(int threads) {};

void gotoblas_affinity_reschedule(void) {};

int get_num_procs(void) { return get_nprocs(); }

int get_num_nodes(void) { return 1; }

int get_node(void) { return 1;}
#endif


