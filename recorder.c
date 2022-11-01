#include <stdio.h>
#include <stdlib.h> 
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <inttypes.h>
#include <stdint.h>
#include "recorder.h"

#define mfence()   asm volatile("mfence" ::: "memory")

//#define DEBUG
#ifdef DEBUG
  #define D if(1)
#else
  #define D if(0)
#endif

#define CHUNK_ENABLED

//extern volatile tier_manager_t *g_tier_manager;
extern tier_manager_t g_tier_manager;
static struct timespec timestamp;

void initialize_recorder(void)
{
	int i, j, w;

	for(i = 0; i < MAX_OBJECTS; i++){
		g_tier_manager.obj_alloc[i] = 0;
		g_tier_manager.obj_status[i] = -1;
	}
    
    for(i=0; i< MAX_OBJECTS; i++){
        g_tier_manager.obj_vector[i].start_addr = -1;
        g_tier_manager.obj_vector[i].metrics.stores_count = 0;
        g_tier_manager.obj_vector[i].sliced = 0;
        
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
    int total_obj;
    unsigned long long remnant_size;
    unsigned long aux;
    int i=0;

#ifdef CHUNK_ENABLED
    if(size > CHUNK_SIZE){
        total_obj = size/CHUNK_SIZE;
        remnant_size = size - (total_obj * CHUNK_SIZE);
        
        while(i < total_obj){
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
//int insert_object(int pid, unsigned long start_addr, unsigned long size)
{
    clock_gettime(CLOCK_REALTIME, &timestamp);
    static int index = 0 ; //get last index assigned
    //this loop will be in infinit loop if the number objects is bigger than MAX_OBJECTS
    while(g_tier_manager.obj_alloc[index] == 1 || g_tier_manager.obj_status[index] != -1){
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
        
    //D fprintf(stderr,"[recorder] insert_object (%lu.%lu, %d, %p, %ld) \n",
    D fprintf(stderr,"recorder_insert, %lu.%lu, %d, %d, %p, %ld\n", \
              timestamp.tv_sec, \
              timestamp.tv_nsec, \
              pid,\
              g_tier_manager.obj_vector[index].obj_index, \
              g_tier_manager.obj_vector[index].start_addr, \
              g_tier_manager.obj_vector[index].size);
    D fflush(stderr);
    index ++;
}

int remove_object(int pid, unsigned long start_addr, unsigned long size)
{
    int i;
    for(i = 0; i < MAX_OBJECTS; i++){
    	if(g_tier_manager.obj_vector[i].start_addr == start_addr && g_tier_manager.obj_vector[i].size == size && g_tier_manager.obj_alloc[i] == 1){
            //D fprintf(stderr,"[recorder] remove_object (%lu.%lu, %d, %p, %ld) \n",
            D fprintf(stderr,"recorder_remove, %lu.%lu, %d, %d, %p, %ld\n", \
                      timestamp.tv_sec, \
                      timestamp.tv_nsec, \
                      pid, \
                      g_tier_manager.obj_vector[i].obj_index, \
                      start_addr, size);
            D fflush(stderr);
            g_tier_manager.obj_alloc[i] = 0;
            
            return 1;
    	}
    }
    
    return 0;
}

