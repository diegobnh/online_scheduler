#include <stdio.h>
#include <stdlib.h> 
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <inttypes.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "recorder.h"


#define mfence()   asm volatile("mfence" ::: "memory")

#define CHUNK_ENABLED

#ifndef INIT_DATAPLACEMENT
#error "INIT_DATAPLACEMENT is not defined."
#endif

#if !(INIT_DATAPLACEMENT >= 1 && INIT_DATAPLACEMENT <= 4)
#error "INIT_DATAPLACEMENT value invalid."
#endif

static int g_pipe_write_fd;
extern tier_manager_t g_tier_manager;
extern double g_start_free_DRAM;
extern int g_app_pid;

int static guard(int ret, char *err){
    if (ret == -1)
    {
        perror(err);
        return -1;
    }
    return ret;
}
double static get_my_timestamp(void){
    char buf[256];
    char cmd[256];
    double timestamp;
    FILE *stream_file;

    sprintf(cmd, "awk '{print $1}' /proc/uptime");

    if (NULL == (stream_file = popen(cmd, "r"))) { perror("popen"); exit(EXIT_FAILURE);
    }

    fgets(buf, sizeof(buf), stream_file);
    sscanf(buf, "%lf", &timestamp);

    return timestamp;
}
void recorder_open_pipes(void){
    char FIFO_PATH_MIGRATION[50];
    char FIFO_PATH_MIGRATION_ERROR[50];
    
    sprintf(FIFO_PATH_MIGRATION, "migration.pipe");
    g_pipe_write_fd = guard(open(FIFO_PATH_MIGRATION, O_WRONLY), "[recorder] Could not open pipe MIGRATION for writing");
}
void static bind_order(unsigned long start_addr, unsigned long size ,int target_node, int obj_index, int type){
    data_bind_t data;
    
    data.obj_index = obj_index;
    data.start_addr = start_addr;
    data.size = size;
    data.nodemask_target_node = 1<<target_node;
    data.type = type;
    data.flag = 1<<0;
    write(g_pipe_write_fd, &data, sizeof(data_bind_t));
}

void initialize_recorder(void)
{
    int i, j, w;

    for(i = 0; i < MAX_OBJECTS; i++){
        g_tier_manager.obj_alloc[i] = 0;
    }
    g_tier_manager.total_dram_mapped_gb = 0;
    g_tier_manager.total_obj = 0;

    for(i=0; i< MAX_OBJECTS; i++){
        g_tier_manager.obj_vector[i].start_addr = -1;
        g_tier_manager.obj_vector[i].metrics.stores_count = 0;
        g_tier_manager.obj_vector[i].sliced = 0;
        g_tier_manager.obj_vector[i].last_decision = -1;
        
        for(j=0 ; j< MEM_LEVELS; j++){
            g_tier_manager.obj_vector[i].metrics.sum_latency_cost[j] = 0;
            g_tier_manager.obj_vector[i].metrics.loads_count[j] = 0;
            g_tier_manager.obj_vector[i].metrics.tlb_hit[j] = 0;
            g_tier_manager.obj_vector[i].metrics.tlb_miss[j] = 0;
        }
    }
}

int insert_object(int pid, unsigned long start_addr, unsigned long size)
{
    int total_chunks;
    unsigned long long remnant_size;
    unsigned long aux;
    int i=0;
    
#ifdef CHUNK_ENABLED
    if(size > CHUNK_SIZE){
        total_chunks = size/CHUNK_SIZE;
        remnant_size = size - (total_chunks * CHUNK_SIZE);
        
        while(i < total_chunks){
           _insert_object(pid, start_addr + (i * CHUNK_SIZE), CHUNK_SIZE, 1);
           i++;
        }
        if(remnant_size > 0){
            _insert_object(pid, start_addr + (i * CHUNK_SIZE), remnant_size, 1);
        }else{
            _insert_object(pid, start_addr + (i * CHUNK_SIZE), CHUNK_SIZE, 1);
        }
    }else{
        _insert_object(pid, start_addr, size, 0);
    }
#else
    _insert_object(pid, start_addr, size, 0);
#endif
    
}

int _insert_object(int pid, unsigned long start_addr, unsigned long size, int sliced)
{
    //clock_gettime(CLOCK_REALTIME, &g_timestamp);
    static int index = 0 ; //get last index assigned
    //this loop will be in infinit loop if the number objects is bigger than MAX_OBJECTS
    while(g_tier_manager.obj_alloc[index] == 1){
    	index ++;
    	//index = index % MAX_OBJECTS;
        if(index == MAX_OBJECTS){
            fprintf(stderr, "Please..increase the variable MAX_OBJECTS!!!\n");
            exit(-1);
        }
    }
    
    g_tier_manager.obj_vector[index].start_addr = start_addr;
    g_tier_manager.obj_vector[index].end_addr = start_addr + size;
    g_tier_manager.obj_vector[index].size = size;
    g_tier_manager.obj_vector[index].pages = size / getpagesize();
    g_tier_manager.obj_vector[index].pid = pid;
    g_tier_manager.obj_vector[index].obj_index = index ;
    g_tier_manager.obj_vector[index].sliced = sliced;
    g_tier_manager.obj_alloc[index] = 1 ;
    g_tier_manager.obj_vector[index].birth_date = get_my_timestamp();
    g_tier_manager.total_obj++;
    
    fprintf(stderr,"[recorder] mmap, %lf, %d, %p, %ld\n", \
                      get_my_timestamp(), \
                      g_tier_manager.obj_vector[index].obj_index, \
                      start_addr, \
                      size);
    
    bind_order(start_addr, size, NODE_0_DRAM, index, INITIAL_DATAPLACEMENT);
    
    fflush(stderr);
    index ++;
}

int deallocate_object(int pid, unsigned long start_addr, unsigned long size){
    int total_chunks;
    unsigned long long remnant_size;
    unsigned long aux;
    int i=0;

#ifdef CHUNK_ENABLED
    if(size > CHUNK_SIZE){
        total_chunks = size/CHUNK_SIZE;
        remnant_size = size - (total_chunks * CHUNK_SIZE);
        
        while(i < total_chunks){
           _deallocate_object(pid, start_addr + (i * CHUNK_SIZE), CHUNK_SIZE, 1);
           i++;
        }
        if(remnant_size > 0){
            _deallocate_object(pid, start_addr + (i * CHUNK_SIZE), remnant_size, 1);
        }else{
            _deallocate_object(pid, start_addr + (i * CHUNK_SIZE), CHUNK_SIZE, 1);
        }
    }else{
        _deallocate_object(pid, start_addr, size, 0);
    }
#else
    _deallocate_object(pid, start_addr, size, 0);
#endif

}

int _deallocate_object(int pid, unsigned long start_addr, unsigned long size, int sliced)
{
    int i;

    //for(i = 0; i < MAX_OBJECTS; i++){
    for(i = 0; i < g_tier_manager.total_obj; i++){
    	if(g_tier_manager.obj_vector[i].start_addr == start_addr && g_tier_manager.obj_vector[i].size == size && g_tier_manager.obj_alloc[i] == 1){
#ifdef DEBUG
            fprintf(stderr,"[recorder] munmap, %lf, %d, %p, %ld\n", \
                              get_my_timestamp(), \
                              g_tier_manager.obj_vector[i].obj_index, \
                              start_addr, \
                              size);
#endif
            fflush(stderr);
            g_tier_manager.obj_alloc[i] = 0;

            return 1;
    	}
    }
    return 0;
}

