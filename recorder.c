#include <stdio.h>
#include <stdlib.h> 
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <inttypes.h>
#include "recorder.h"

#define DEBUG
#ifdef DEBUG
  #define D if(1)
#else
  #define D if(0)
#endif

void initialize_recorder(struct schedule_manager *args)
{
	int i, j, w;
	
	for(i = 0; i < MAX_OBJECTS; i++){
		args->tier[0].obj_flag_alloc[i] = 0;
		args->tier[1].obj_flag_alloc[i] = 0;
	}
	args->tier[0].current_obj_index = 0;
	args->tier[1].current_obj_index = 0;
	
	args->tier[0].num_obj = 0;
	args->tier[1].num_obj = 0;
    
	args->tier[0].current_memory_consumption = 0;
	args->tier[1].current_memory_consumption = 0;
    
    for(i=0; i< MAX_OBJECTS; i++){
        for(w=0;w<RING_BUFFER_SIZE;w++){
            args->tier[0].obj_vector[i].ring.stores_count[w] = 0;
            args->tier[1].obj_vector[i].ring.stores_count[w] = 0;
            
            for(j=0 ; j< MEM_LEVELS; j++){
                args->tier[0].obj_vector[i].ring.sum_latency_cost[w][j] = 0;
                args->tier[0].obj_vector[i].ring.loads_count[w][j] = 0;
                args->tier[0].obj_vector[i].ring.TLB_hit[w][j] = 0;
                args->tier[0].obj_vector[i].ring.TLB_miss[w][j] = 0;
                
                args->tier[1].obj_vector[i].ring.sum_latency_cost[w][j] = 0;
                args->tier[1].obj_vector[i].ring.loads_count[w][j] = 0;
                args->tier[1].obj_vector[i].ring.TLB_hit[w][j] = 0;
                args->tier[1].obj_vector[i].ring.TLB_miss[w][j] = 0;
            }
        }
    }
   
}
int insert_allocation_on_pmem(struct schedule_manager *args, int pid, unsigned long start_addr, unsigned long size)
{
    D fprintf(stderr,"\t[recorder] insert_allocation_on_pmem Try to get lock!!\n");
    pthread_mutex_lock(&args->global_mutex);
    //D fprintf(stderr, "\t[recorder] insert_allocation_on_tier[1] from pid:%d\n", pid);
    
    int index = args->tier[1].current_obj_index ; //get last index assigned
    //this loop will be in infinit loop if the number objects is bigger than MAX_OBJECTS
    while(args->tier[1].obj_flag_alloc[index] == 1){
        index ++;
        index = index % MAX_OBJECTS;
    }
    //D fprintf(stderr, "\t[recorder] insert %p on tier[1] index: %d\n",start_addr, index);
    
    args->tier[1].obj_vector[index].start_addr = start_addr;
    args->tier[1].obj_vector[index].end_addr = start_addr + size;
    args->tier[1].obj_vector[index].size = size;
    args->tier[1].obj_vector[index].pages = size / getpagesize();
    args->tier[1].obj_vector[index].pid = pid;
    args->tier[1].obj_vector[index].index_id = index; //save which position this allocation was saved
    
    args->tier[1].obj_flag_alloc[index] = 1 ; //check to say that this position is busy now
    args->tier[1].num_obj++;
    args->tier[1].current_obj_index = index ; //this value will be the first index to check if is available
    args->tier[1].current_memory_consumption += size;
    
    pthread_mutex_unlock(&args->global_mutex);
    D fprintf(stderr,"\t[recorder] insert_allocation_on_mem Free lock!!\n");

}
int insert_allocation_on_dram(struct schedule_manager *args, int pid, unsigned long start_addr, unsigned long size)
{
    D fprintf(stderr,"\t[recorder] insert_allocation_on_dram Try to get lock!!\n");
	pthread_mutex_lock(&args->global_mutex);
    //D fprintf(stderr, "\t[recorder] insert_allocation_on_tier[0] from pid:%d\n", pid);
    
    int index = args->tier[0].current_obj_index ; //get last index assigned
    //this loop will be in infinit loop if the number objects is bigger than MAX_OBJECTS
    while(args->tier[0].obj_flag_alloc[index] == 1){
    	index ++;
    	index = index % MAX_OBJECTS;
    }
    //D fprintf(stderr, "\t[recorder] insert %p on tier[0] index: %d\n",start_addr, index);
    
	args->tier[0].obj_vector[index].start_addr = start_addr;
	args->tier[0].obj_vector[index].end_addr = start_addr + size;
	args->tier[0].obj_vector[index].size = size;
	args->tier[0].obj_vector[index].pages = size / getpagesize();
    args->tier[0].obj_vector[index].pid = pid;
    args->tier[0].obj_vector[index].index_id = index;
    
    args->tier[0].obj_flag_alloc[index] = 1 ;
    args->tier[0].num_obj++;
    args->tier[0].current_obj_index = index ;
    args->tier[0].current_memory_consumption += size;
    
    pthread_mutex_unlock(&args->global_mutex);
    D fprintf(stderr,"\t[recorder] insert_allocation_on_dram Free lock!!\n");
}
int remove_allocation_on_pmem(struct schedule_manager *shared_memory, int pid, unsigned long start_addr, unsigned long size)
{
    D fprintf(stderr,"\t[recorder] remove_allocation_on_pmem Try to get lock!!\n");
    pthread_mutex_lock(&shared_memory->global_mutex);
    
    int i;
    for(i = 0; i < MAX_OBJECTS; i++){
    	if(shared_memory->tier[1].obj_vector[i].start_addr == start_addr && shared_memory->tier[1].obj_vector[i].size == size && shared_memory->tier[1].obj_flag_alloc[i] == 1){
            //D fprintf(stderr,"\t[recorder] removed %p from tier[1] index:%d\n",start_addr, i);
            shared_memory->tier[1].obj_flag_alloc[i] = 0;
            shared_memory->tier[1].current_memory_consumption -= size;
            
            D fprintf(stderr,"\t[recorder] remove_allocation_on_pmem Free lock!!\n");
            pthread_mutex_unlock(&shared_memory->global_mutex);
    		return 1;
    	}
    }
    
	pthread_mutex_unlock(&shared_memory->global_mutex);
    D fprintf(stderr,"\t[recorder] remove_allocation_on_pmem Free lock!!\n");
    return 0;
	
}	
int remove_allocation_on_dram(struct schedule_manager *shared_memory, int pid, unsigned long start_addr, unsigned long size)
{
    D fprintf(stderr,"\t[recorder] remove_allocation_on_dram Try to get lock!!\n");
    pthread_mutex_lock(&shared_memory->global_mutex);

    int i;
    for(i = 0; i < MAX_OBJECTS; i++){
    	if(shared_memory->tier[0].obj_vector[i].start_addr == start_addr && shared_memory->tier[0].obj_vector[i].size == size && shared_memory->tier[0].obj_flag_alloc[i] == 1){
            //D fprintf(stderr,"\t[recorder] removed %p from tier[0] index:%d\n",start_addr, i);
            shared_memory->tier[0].obj_flag_alloc[i] = 0;
            shared_memory->tier[0].current_memory_consumption -= size;
            D fprintf(stderr,"\t[recorder] remove_allocation_on_dram Free lock!!\n");
            pthread_mutex_unlock(&shared_memory->global_mutex);
    		return 1;
    	}
    }
	pthread_mutex_unlock(&shared_memory->global_mutex);
    D fprintf(stderr,"\t[recorder] remove_allocation_on_dram Free lock!!\n");
    return 0;
    
}

