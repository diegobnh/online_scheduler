#define _GNU_SOURCE 1
#include <linux/perf_event.h>
#include <perf/core.h>
#include <perf/cpumap.h>
#include <perf/event.h>
#include <perf/evlist.h>
#include <perf/evsel.h>
#include <perf/mmap.h>
#include <perf/threadmap.h>
#include <pthread.h>
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

#include "recorder.h"
#define rmb()   asm volatile("lfence" ::: "memory")

//#define DEBUG
#ifdef DEBUG
  #define D if(1)
#else
  #define D if(0)
#endif

//extern volatile tier_manager_t *g_tier_manager;
extern tier_manager_t g_tier_manager;
extern volatile sig_atomic_t g_running;

struct __attribute__((__packed__)) sample {
    uint32_t pid, tid;
    uint32_t size;
    char data[];
};

struct trace_entry {
    unsigned short type;
    unsigned char flags;
    unsigned char preempt_count;
    int pid;
};

struct syscall_mmap_enter_args {
    struct trace_entry ent;
    int sys_nr;
    unsigned long addr;
    unsigned long len;
    unsigned long prot;
    unsigned long flags;
    unsigned long fd;
    unsigned long off;
};

struct syscall_munmap_enter_args {
    struct trace_entry ent;
    int sys_nr;
    unsigned long addr;
    unsigned long len;
};

struct syscall_mmap_exit_args {
    struct trace_entry ent;
    int sys_nr;
    long ret;
};

//static const int sys_enter_mmap_id = 99;       // see: /sys/kernel/debug/tracing/events/syscalls/sys_enter_mmap/id
//static const int sys_exit_mmap_id = 98;        // see: /sys/kernel/debug/tracing/events/syscalls/sys_exit_mmap/id
//static const int sys_enter_munmap_id = 576;     // see: /sys/kernel/debug/tracing/events/syscalls/sys_enter_munmap/id

static int sys_enter_mmap_id ;
static int sys_exit_mmap_id ;
static int sys_enter_munmap_id ;     


void read_mmaps_ids(void){
    FILE *fp;
    
    fp = fopen("/sys/kernel/debug/tracing/events/syscalls/sys_enter_mmap/id", "r");
    if(fp != NULL){
        fscanf(fp, "%d", &sys_enter_mmap_id);
        fclose(fp);
    }
    
    fp = fopen("/sys/kernel/debug/tracing/events/syscalls/sys_exit_mmap/id", "r");
    if(fp != NULL){
        fscanf(fp, "%d", &sys_exit_mmap_id);
        fclose(fp);
    }
    fp = fopen("/sys/kernel/debug/tracing/events/syscalls/sys_enter_munmap/id", "r");
    if(fp != NULL){
        fscanf(fp, "%d", &sys_enter_munmap_id);
        fclose(fp);
    }
    
}
static int libperf_print(enum libperf_print_level level, const char *fmt, va_list ap)
{
    return vfprintf(stderr, fmt, ap);
}
void *thread_intercept_mmap(void){

    struct perf_evlist *evlist;
    struct perf_evsel *evsel;
    struct perf_mmap *map;
    struct perf_cpu_map *cpus;
    union perf_event *event;
    int err;
    
    read_mmaps_ids();
        
    struct perf_event_attr attr = {
        .type = PERF_TYPE_TRACEPOINT,
        .disabled = 1,
        .precise_ip = 2,
        .exclude_kernel = 1,
        .exclude_user = 0,
        .exclude_hv = 1,
        .wakeup_events = 1,
        .sample_period = 1,
        .sample_type = PERF_SAMPLE_TID | PERF_SAMPLE_RAW,
    };
    
    libperf_init(libperf_print);
    
    cpus = perf_cpu_map__new(NULL);
    if (!cpus) {
        fprintf(stderr, "failed to create cpus\n");
        //return -1;
        exit(-1);
    }
    
    evlist = perf_evlist__new();
    if (!evlist) {
        fprintf(stderr, "failed to create evlist\n");
        goto out_cpus;
    }
    // event: sys_enter_mmap
    attr.config = sys_enter_mmap_id;
    evsel = perf_evsel__new(&attr);
    if (!evsel) {
        fprintf(stderr, "failed to create event\n");
        goto out_cpus;
    }
    perf_evlist__add(evlist, evsel);
    
    // event: sys_exit_mmap
    attr.config = sys_exit_mmap_id;
    evsel = perf_evsel__new(&attr);
    if (!evsel) {
        fprintf(stderr, "failed to create event\n");
        goto out_cpus;
    }
    perf_evlist__add(evlist, evsel);
    
    // event: sys_enter_munmap
    attr.config = sys_enter_munmap_id;
    evsel = perf_evsel__new(&attr);
    if (!evsel) {
        fprintf(stderr, "failed to create event\n");
        goto out_cpus;
    }
    perf_evlist__add(evlist, evsel);
    
    perf_evlist__set_maps(evlist, cpus, NULL);
    
    err = perf_evlist__open(evlist);
    if (err) {
        fprintf(stderr, "failed to open evlist\n");
        goto out_evlist;
    }
    
    err = perf_evlist__mmap(evlist, 4);
    if (err) {
        fprintf(stderr, "failed to mmap evlist\n");
        goto out_evlist;
    }
    
    struct syscall_mmap_enter_args *mmap_enter_args;
    
    perf_evlist__enable(evlist);
        
    while (g_running) {
        perf_evlist__poll(evlist, -1);
        
        perf_evlist__for_each_mmap(evlist, map, false) {
            
            if (perf_mmap__read_init(map) < 0)
                continue;
            
            rmb();
            
            while ((event = perf_mmap__read_event(map)) != NULL) {
                const __u32 type = event->header.type;
                
                if (type != PERF_RECORD_SAMPLE) {
                    //printf("other type: %d\n", type);
                    //return -1;
                    perf_mmap__consume(map);
                    continue;
                }
                
                struct sample *sample = (struct sample *) event->sample.array;
                struct trace_entry *entry = (struct trace_entry *) sample->data;
                int sys_type = entry->type;
                
                if (sys_type == sys_enter_mmap_id) {
                    mmap_enter_args = (struct syscall_mmap_enter_args *) sample->data;
                    //D printf("sys_enter_mmap(pid=%d,tid=%d) - addr: 0x%lx, len: 0x%lx,\
                        prot: 0x%lx, flags: 0x%lx, fd: 0x%lx, off: 0x%lx\n",\
                           sample->pid, sample->tid, mmap_enter_args->addr, mmap_enter_args->len, mmap_enter_args->prot,\
                           mmap_enter_args->flags, mmap_enter_args->fd, mmap_enter_args->off);
                    
                } else if (sys_type == sys_exit_mmap_id) {
                    struct syscall_mmap_exit_args *args = (struct syscall_mmap_exit_args *) sample->data;
                    
                    // was it annon?
                    if (mmap_enter_args->addr == 0) {
                        //if(g_tier_manager.pids_to_manager[0] == sample->pid){
                        if(g_tier_manager.pids_to_manager[0] == sample->pid && mmap_enter_args->flags != 0x4022){
                            D printf("sys_enter_mmap(pid=%d,tid=%d) - addr: 0x%lx, len: 0x%lx,\
                                prot: 0x%lx, flags: 0x%lx, fd: 0x%lx, off: 0x%lx\n",\
                                   sample->pid, sample->tid, mmap_enter_args->addr, mmap_enter_args->len, mmap_enter_args->prot,\
                                   mmap_enter_args->flags, mmap_enter_args->fd, mmap_enter_args->off);
                            //D printf("sys_exit_mmap(pid=%d,tid=%d) - ret=%ld\n", sample->pid, sample->tid, args->ret);
                            insert_object(sample->pid, args->ret, mmap_enter_args->len);
                        }
                        
                    }
                    
                } else if (sys_type == sys_enter_munmap_id) {
                    struct syscall_munmap_enter_args *args = (struct syscall_munmap_enter_args *) sample->data;
                    
                    if(g_tier_manager.pids_to_manager[0] == sample->pid){
                        D printf("sys_enter_munmap(pid=%d,tid=%d) - addr: 0x%lx, len: 0x%lx\n",
                               sample->pid, sample->tid, args->addr, args->len);
                        remove_object(sample->pid, args->addr, args->len);
                    }
                    
                    
                }
                perf_mmap__consume(map);
            }
            perf_mmap__read_done(map);
            
        }
        /*
        fprintf(stderr, "[intercept mmap] Total Objs:%d\n", g_tier_manager.total_objs);
        for(int i=0;i<MAX_OBJECTS;i++){
            //if(g_tier_manager.obj_vector[i].metrics.loads_count[4] > MINIMUM_LLCM && g_tier_manager.obj_alloc[i] == 1){
            if(g_tier_manager.obj_alloc[i] == 1 ){
                fprintf(stderr, "Obj:%d Start_addr:%p\n", \
                        g_tier_manager.obj_vector[i].obj_index, \
                        g_tier_manager.obj_vector[i].start_addr);
                
            }
        }
        */
    }
    
    
out_evlist:
    perf_evlist__delete(evlist);
out_cpus:
    perf_cpu_map__put(cpus);
    
    //return err;
}
