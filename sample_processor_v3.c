#define _GNU_SOURCE 1

#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <stdint.h>
#include <errno.h>

#include "recorder.h"
#include "time.h"
#define ALPHA 0.9

//#define DEBUG
#ifdef DEBUG
  #define D if(1)
#else
  #define D if(0)
#endif


int g_total_dram_objs;//save current total objects on DRAM from shared memory
int g_total_pmem_objs;//save current total objects on PMEM from shared memory


void calc_moving_average(struct schedule_manager *args){
	int i, j;
    struct timespec start, end;
    double old_value;
    double curr_value;
	
    fprintf(stderr, "Inicio do calc movind averaging\n");
    //alocate the number of total objects, even if exist deallocated
    g_total_dram_objs = args->tier[0].num_obj;
    g_total_pmem_objs = args->tier[1].num_obj;
    
    //first i copy all date from ring buffer to local variable
    if(g_total_dram_objs > 0 ){
        
        for(i=0; i< g_total_dram_objs; i++){
            for(j=0; j< MEM_LEVELS; j++){
                fprintf(stderr, "DRAM i=%d, j=%d\n", i, j);
                old_value = args->tier[0].obj_vector[i].metrics.sum_latency_cost[j];
                curr_value = args->tier[0].obj_vector[i].samples.sum_latency_cost[j];
                args->tier[0].obj_vector[i].metrics.sum_latency_cost[j] = (curr_value * (1-ALPHA)) + (old_value * ALPHA);
                
                old_value = args->tier[0].obj_vector[i].metrics.loads_count[j];
                curr_value = args->tier[0].obj_vector[i].samples.loads_count[j];
                args->tier[0].obj_vector[i].metrics.loads_count[j] = (curr_value * (1-ALPHA)) + (old_value * ALPHA) ;
                
                old_value = args->tier[0].obj_vector[i].metrics.TLB_hit[j];
                curr_value = args->tier[0].obj_vector[i].samples.TLB_hit[j];
                args->tier[0].obj_vector[i].metrics.TLB_hit[j] = (curr_value * (1-ALPHA)) + (old_value * ALPHA) ;
                
                old_value = args->tier[0].obj_vector[i].metrics.TLB_miss[j];
                curr_value = args->tier[0].obj_vector[i].samples.TLB_miss[j];
                args->tier[0].obj_vector[i].metrics.TLB_miss[j] = (curr_value * (1-ALPHA)) + (old_value * ALPHA) ;
                
            }
            old_value = args->tier[0].obj_vector[i].metrics.stores_count;
            curr_value = args->tier[0].obj_vector[i].samples.stores_count;
            args->tier[0].obj_vector[i].metrics.stores_count = (curr_value * (1-ALPHA)) + (old_value * ALPHA) ;
        }
    }
    
    if(g_total_pmem_objs > 0 ){
       
        for(i=0; i< g_total_pmem_objs; i++){
            for(j=0; j< MEM_LEVELS; j++){
                fprintf(stderr, "PMEM i=%d, j=%d\n", i, j);
                old_value = args->tier[1].obj_vector[i].metrics.sum_latency_cost[j];
                curr_value = args->tier[1].obj_vector[i].samples.sum_latency_cost[j];
                args->tier[1].obj_vector[i].metrics.sum_latency_cost[j] = (curr_value * (1-ALPHA)) + (old_value * ALPHA);
                
                old_value = args->tier[1].obj_vector[i].metrics.loads_count[j];
                curr_value = args->tier[1].obj_vector[i].samples.loads_count[j];
                args->tier[1].obj_vector[i].metrics.loads_count[j] = (curr_value * (1-ALPHA)) + (old_value * ALPHA) ;
                
                old_value = args->tier[1].obj_vector[i].metrics.TLB_hit[j];
                curr_value =  args->tier[1].obj_vector[i].samples.TLB_hit[j];
                args->tier[1].obj_vector[i].metrics.TLB_hit[j] = (curr_value * (1-ALPHA)) + (old_value * ALPHA) ;
                
                old_value = args->tier[1].obj_vector[i].metrics.TLB_miss[j];
                curr_value =  args->tier[1].obj_vector[i].samples.TLB_miss[j];
                args->tier[1].obj_vector[i].metrics.TLB_miss[j] = (curr_value * (1-ALPHA)) + (old_value * ALPHA) ;
            }
            old_value = args->tier[1].obj_vector[i].metrics.stores_count;
            curr_value =  args->tier[1].obj_vector[i].samples.stores_count;
            args->tier[1].obj_vector[i].metrics.stores_count = (curr_value * (1-ALPHA)) + (old_value * ALPHA) ;
        }
    }
}
