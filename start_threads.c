#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <stdint.h>
#include "recorder.h"
#include "intercept_mmap.h"
#include "monitor.h"
#include "actuator.h"
#include "track_mapping.h"
#include <stdatomic.h>
#include <sys/syscall.h>
#define mfence()   asm volatile("mfence" ::: "memory")

pthread_t intercept_mmap;
pthread_t monitor;
pthread_t actuator;
pthread_t track_mapping;

tier_manager_t g_tier_manager;
volatile sig_atomic_t g_running = 1;

float g_actuator_interval;
float g_monitor_interval;
float g_track_mapping_interval;
int g_hotness_threshold = 10;

struct timespec g_start, g_end;

void close_start_threads(int signum, siginfo_t *info, void *uc){
    g_running = 0;
}
int read_enviroment_variables(void){
    char* actuator_interval = getenv("ACTUATOR_INTERVAL");
    char* monitor_interval = getenv("MONITOR_INTERVAL");
    char* track_mapping_interval = getenv("TRACK_MAPPING_INTERVAL");
    
        
    if(!(actuator_interval || monitor_interval || track_mapping_interval)){
        fprintf(stderr, "You are missing some environment variable !!\n");
        exit(-1);
    }else{
        sscanf(actuator_interval, "%f", &g_actuator_interval);
        sscanf(monitor_interval, "%f", &g_monitor_interval);
        sscanf(track_mapping_interval, "%f", &g_track_mapping_interval);
        
        fprintf(stderr,"ACTUATOR_INTERVAL(sec)=%.2lf \
                \nMONITOR_INTERVAL(sec)=%.2lf \
                \nTRACK_MAPPING_INTERVAL(sec)=%.2lf\n", \
                g_actuator_interval, \
                g_monitor_interval, \
                g_track_mapping_interval);
    }
            
}
int main(int argc, char *argv[]){
    int i;
    int pid;
    int cont = 0;    
    
    clock_gettime(CLOCK_REALTIME, &g_start);
    /*
    We are not starting the application before, because we lost some allocations in the beggining of execution.
    if(argc < 2){
        fprintf(stderr, "You should pass the pid to monitore!!\n");
        fprintf(stderr, "Example: myprogram pid1 pid2 pid3\n");
        return 1;
    }else{
        for(i=1; i<argc; i++){
            sscanf(argv[i], "%d", &pid);
            g_tier_manager.pids_to_manager[cont] = pid;
            cont++;
        }
    }
    */
    read_enviroment_variables();

    struct sigaction sa;
    sa.sa_sigaction = close_start_threads;
    sa.sa_flags = SIGUSR1;
    
    if (sigaction(SIGUSR1, &sa, NULL) < 0) { 
        fprintf(stderr,"Error setting up signal handler\n");
        return 1;
    }

    initialize_recorder();

    pthread_create(&intercept_mmap, NULL, thread_intercept_mmap, NULL);
    //pthread_create(&monitor, NULL, thread_monitor, NULL);
    //pthread_create(&actuator, NULL, thread_actuator, NULL);
    //pthread_create(&track_mapping, NULL, thread_track_mapping, NULL);

    pthread_join(intercept_mmap, NULL);
    clock_gettime(CLOCK_REALTIME, &g_end);
    fprintf(stderr, "\n[start_threads] Join intercept mmap\n");
    
    //pthread_join(monitor, NULL);
    //fprintf(stderr, "[start_threads] Join monitor\n");
    
    //pthread_join(actuator, NULL);
    //fprintf(stderr, "[start_threads] Join atuador\n");
    
    //pthread_join(track_mapping, NULL);
    //fprintf(stderr, "[start_threads] Join track decisions\n");
    
    uint64_t delta_us = (g_end.tv_sec - g_start.tv_sec) * 1000000 + (g_end.tv_nsec - g_start.tv_nsec) / 1000;
    fprintf(stderr, "Execution_time(sec):%.2lf\n", delta_us/1000000.0);
    
    return 0;
                
}
