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
#include <sys/types.h>
#include <sys/stat.h>
#define mfence()   asm volatile("mfence" ::: "memory")

pthread_t intercept_mmap;
pthread_t monitor;
pthread_t actuator;
pthread_t track_mapping;

tier_manager_t g_tier_manager;

//When i was using only one variable to all threads, sometimes actuator and monitor had finished but intercept still send new allocations
volatile sig_atomic_t g_running_intercept = 1;
volatile sig_atomic_t g_running_actuator = 1;
volatile sig_atomic_t g_running_monitor = 1;

float g_actuator_interval;
float g_monitor_interval;
float g_hotness_threshold;
int g_sample_freq;
double g_start_free_DRAM;
int g_app_pid = -1;
struct timespec g_start, g_end;

int static guard(int ret, char *err){
    if (ret == -1)
    {
        perror(err);
        return -1;
    }
    return ret;
}
void close_start_threads(int signum, siginfo_t *info, void *uc){
    g_running_intercept = 0;
    //sleep(1);
    g_running_actuator = 0;
    //sleep(1);
    g_running_monitor = 0;
}

void static start_threads_open_pipes(void)
{
    char FIFO_PATH_MIGRATION[50] ;
    char FIFO_PATH_MIGRATION_ERROR[50] ;

    sprintf(FIFO_PATH_MIGRATION, "migration.pipe");
    sprintf(FIFO_PATH_MIGRATION_ERROR, "migration_error.pipe");
 
    system("rm -f migration*.pipe");
    
    guard(mkfifo(FIFO_PATH_MIGRATION, 0777), "Could not create pipe");
    guard(mkfifo(FIFO_PATH_MIGRATION_ERROR, 0777), "Could not create pipe");

}

void static set_current_free_dram(void){
    char buf[256];
    char cmd[256];
    FILE *stream_file;

    sprintf(cmd, "grep MemFree /sys/devices/system/node/node0/meminfo | awk '{print $4/1000}' ");

    if (NULL == (stream_file = popen(cmd, "r"))) {
        perror("popen");
        exit(EXIT_FAILURE);
    }

    fgets(buf, sizeof(buf), stream_file);
    sscanf(buf, "%lf",&g_start_free_DRAM);//measured in megabytes

    g_start_free_DRAM = (g_start_free_DRAM/1000.0); //convert to GB
    
    fclose(stream_file);
    fprintf(stderr, "\nFree DRAM start:%.4lf(GB)\n",g_start_free_DRAM);
}

int static read_enviroment_variables(void){
    char* monitor_interval = getenv("MONITOR_INTERVAL");
    char* actuator_interval = getenv("ACTUATOR_INTERVAL");
    //char* track_mapping_interval = getenv("TRACK_MAPPING_INTERVAL");
    char* hotness_threshold = getenv("HOTNESS_THRESHOLD");
    char* sample_freq = getenv("SAMPLE_FREQ");
        
    if(!(actuator_interval || monitor_interval || hotness_threshold || sample_freq)){
        fprintf(stderr, "You are missing some environment variable !!\n");
        exit(-1);
    }else{
        sscanf(actuator_interval, "%f", &g_actuator_interval);
        sscanf(monitor_interval, "%f", &g_monitor_interval);
        sscanf(hotness_threshold, "%f", &g_hotness_threshold);
        sscanf(sample_freq, "%d", &g_sample_freq);
        
        fprintf(stderr,"ACTUATOR_INTERVAL(sec)=%.2lf \
                \nMONITOR_INTERVAL(sec)=%.2lf \
                \nHOTNESS_THRESHOLD=%.2lf \
                \nSAMPLE_FREQ=%d\n", \
                g_actuator_interval, \
                g_monitor_interval, \
                g_hotness_threshold, \
                g_sample_freq);
    }
            
}
int main(int argc, char *argv[]){
    int i;
    int cont = 0;
    FILE *fptr;
    
    clock_gettime(CLOCK_REALTIME, &g_start);
   
    read_enviroment_variables();

    struct sigaction sa;
    sa.sa_sigaction = close_start_threads;
    sa.sa_flags = SIGUSR1;
    
    if (sigaction(SIGUSR1, &sa, NULL) < 0) { 
        fprintf(stderr,"Error setting up signal handler\n");
        return 1;
    }

    initialize_recorder();
    start_threads_open_pipes();
    set_current_free_dram();

    pthread_create(&intercept_mmap, NULL, thread_intercept_mmap, NULL);
    pthread_create(&monitor, NULL, thread_monitor, NULL);
    pthread_create(&actuator, NULL, thread_actuator, NULL);

    
    while(g_app_pid == -1){
        fptr = fopen("pid.txt", "r");
        if(fptr != NULL )
        {
            fscanf(fptr, "%d", &g_app_pid);
            fseek(fptr, 0, SEEK_SET);
        }
    }

    pthread_join(intercept_mmap, NULL);
    clock_gettime(CLOCK_REALTIME, &g_end);
    fprintf(stderr, "\n[start_threads] Join intercept mmap\n");
    
    pthread_join(monitor, NULL);
    fprintf(stderr, "[start_threads] Join monitor\n");
    
    pthread_join(actuator, NULL);
    fprintf(stderr, "[start_threads] Join atuador\n");
    
    fprintf(stderr, "[start_threads] Total_obj:%d\n", g_tier_manager.total_obj);
    uint64_t delta_us = (g_end.tv_sec - g_start.tv_sec) * 1000000 + (g_end.tv_nsec - g_start.tv_nsec) / 1000;
    fprintf(stderr, "Execution_time(sec):%.2lf\n", delta_us/1000000.0);
    
    return 0;
                
}
