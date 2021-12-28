#include <stdio.h>
#include <stdlib.h> 
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <inttypes.h>
#include "recorder.h"

#define mfence()   asm volatile("mfence" ::: "memory")

//#define DEBUG
#ifdef DEBUG
  #define D if(1)
#else
  #define D if(0)
#endif

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
        
        for(j=0 ; j< MEM_LEVELS; j++){
            
            g_tier_manager.obj_vector[i].metrics.sum_latency_cost[j] = 0;
            g_tier_manager.obj_vector[i].metrics.loads_count[j] = 0;
            g_tier_manager.obj_vector[i].metrics.TLB_hit[j] = 0;
            g_tier_manager.obj_vector[i].metrics.TLB_miss[j] = 0;
        }
    }
}

int insert_object(int pid, unsigned long start_addr, unsigned long size)
{
    clock_gettime(CLOCK_REALTIME, &timestamp);
    int index = 0 ; //get last index assigned
    //this loop will be in infinit loop if the number objects is bigger than MAX_OBJECTS
    while(g_tier_manager.obj_alloc[index] == 1 || g_tier_manager.obj_status[index] != -1){
    	index ++;
    	index = index % MAX_OBJECTS;
    }
    
	g_tier_manager.obj_vector[index].start_addr = start_addr;
	g_tier_manager.obj_vector[index].end_addr = start_addr + size;
	g_tier_manager.obj_vector[index].size = size;
	g_tier_manager.obj_vector[index].pages = size / getpagesize();
    g_tier_manager.obj_vector[index].pid = pid;
    g_tier_manager.obj_vector[index].obj_index = index ;
    g_tier_manager.obj_alloc[index] = 1 ;
        
    D fprintf(stderr,"\t[recorder] insert_object (%lu.%lu, %d, %p, %ld) \n", \
              timestamp.tv_sec, \
              timestamp.tv_nsec, \
              g_tier_manager.obj_vector[index].obj_index, \
              g_tier_manager.obj_vector[index].start_addr, \
              g_tier_manager.obj_vector[index].size);
    D fflush(stderr);
}

int remove_object(int pid, unsigned long start_addr, unsigned long size)
{
    int i;
    for(i = 0; i < MAX_OBJECTS; i++){
    	if(g_tier_manager.obj_vector[i].start_addr == start_addr && g_tier_manager.obj_vector[i].size == size && g_tier_manager.obj_alloc[i] == 1){
            D fprintf(stderr,"\t[recorder] remove_object (%lu.%lu, %d, %p, %ld) \n", \
                      timestamp.tv_sec, \
                      timestamp.tv_nsec, \
                      g_tier_manager.obj_vector[i].obj_index, \
                      start_addr, size);
            D fflush(stderr);
            g_tier_manager.obj_alloc[i] = 0;
            
            return 1;
    	}
    }
    
    return 0;
}

