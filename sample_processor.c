#define _GNU_SOURCE 1

#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <stdint.h>
#include <errno.h>

#include "recorder.h"
#include "time.h"
#define SLEEP_TIME 10
#define ALPHA 0.9

//#define DEBUG
#ifdef DEBUG
  #define D if(1)
#else
  #define D if(0)
#endif
/*
Simple Moving Average (SMA), Weighted Moving Average (WMA), Exponential Moving Average (EMA)
*/

ring_buffer_t *g_dram_tier_ring;
ring_buffer_t *g_pmem_tier_ring;
metric_t *g_dram_metrics;
metric_t *g_pmem_metrics;

int g_total_dram_objs;//save current total objects on DRAM from shared memory
int g_total_pmem_objs;//save current total objects on PMEM from shared memory

void print_struc_pmem(ring_buffer_t * g_dram_tier_ring){
    int i, j, w;

    for(i=0; i< g_total_pmem_objs; i++){
        fprintf(stderr, "i=%d\n",i);
        for(w=0; w< MEM_LEVELS; w++){
            fprintf(stderr, "\tw=%d\n",w);
            for(j=0; j< RING_BUFFER_SIZE; j++){
                fprintf(stderr, "\t\tj=%d, ",j);
                fprintf(stderr, "lat:%ld, \
                                 load:%ld, \
                                 tlb_miss:%ld,\
                                 tlb_hit:%ld\n", \
                                 g_pmem_tier_ring[i].sum_latency_cost[j][w],\
                                 g_pmem_tier_ring[i].loads_count[j][w],\
                                 g_pmem_tier_ring[i].TLB_hit[j][w],\
                                 g_pmem_tier_ring[i].TLB_miss[j][w]);
            }
        }
    }
}
void print_struc_dram(ring_buffer_t * g_dram_tier_ring){
    int i, j, w;

    for(i=0; i< g_total_dram_objs; i++){
        fprintf(stderr, "i=%d\n",i);
        for(w=0; w< MEM_LEVELS; w++){
            fprintf(stderr, "\tw=%d\n",w);
            for(j=0; j< RING_BUFFER_SIZE; j++){
                fprintf(stderr, "\t\tj=%d, ",j);
                fprintf(stderr, "\t\t\tlat:%ld, \
                                 load:%ld, \
                                 tlb_miss:%ld,\
                                 tlb_hit:%ld\n", \
                                 g_dram_tier_ring[i].sum_latency_cost[j][w],\
                                 g_dram_tier_ring[i].loads_count[j][w],\
                                 g_dram_tier_ring[i].TLB_hit[j][w],\
                                 g_dram_tier_ring[i].TLB_miss[j][w]);
            }
        }
    }
}

int calculate_SMA_for_DRAM(void){
	int i, j, w;
	long long lat, loads, stores, tlb_miss, tlb_hit;
	
    //This is for DRAM tier
	for(i=0; i< g_total_dram_objs; i++){//I control the object
	    stores = 0;
		for(w=0; w< MEM_LEVELS; w++){//control the memory level
	        stores += g_dram_tier_ring->stores_count[w];
	        lat = loads = tlb_miss = tlb_hit = 0;
			for(j=0; j< RING_BUFFER_SIZE; j++){//control the ring buffer position
				lat      += g_dram_tier_ring[i].sum_latency_cost[j][w];
				loads    += g_dram_tier_ring[i].loads_count[j][w];
				tlb_miss += g_dram_tier_ring[i].TLB_hit[j][w];
				tlb_hit  += g_dram_tier_ring[i].TLB_miss[j][w];
			}
            //local variable
            g_dram_metrics[i].sum_latency_cost[w] = (double)lat/RING_BUFFER_SIZE;
            g_dram_metrics[i].loads_count[w] = (double)loads/RING_BUFFER_SIZE;
            g_dram_metrics[i].TLB_hit[w] = (double)tlb_hit/RING_BUFFER_SIZE;
            g_dram_metrics[i].TLB_miss[w] = (double)tlb_miss/RING_BUFFER_SIZE;
           
		}
        g_dram_metrics[i].stores_count = (double)stores/RING_BUFFER_SIZE;
	}
}

int calculate_SMA_for_PMEM(void){
    int i, j, w;
    long long lat, loads, stores, tlb_miss, tlb_hit;
    
	//This is for PMEM tier
	for(i=0; i< g_total_pmem_objs; i++){
	    stores = 0;
		for(w=0; w< MEM_LEVELS; w++){ 	    
	        stores += g_pmem_tier_ring->stores_count[w];
	        lat = loads = tlb_miss = tlb_hit = 0;
   
			for(j=0; j< RING_BUFFER_SIZE; j++){ 
				lat      += g_pmem_tier_ring[i].sum_latency_cost[j][w];
				loads    += g_pmem_tier_ring[i].loads_count[j][w];
				tlb_miss += g_pmem_tier_ring[i].TLB_hit[j][w];
				tlb_hit  += g_pmem_tier_ring[i].TLB_miss[j][w];
			}
			//local variable
            g_pmem_metrics[i].sum_latency_cost[w] = (double)lat/RING_BUFFER_SIZE;
            g_pmem_metrics[i].loads_count[w] = (double)loads/RING_BUFFER_SIZE;
            g_pmem_metrics[i].TLB_hit[w] = (double)tlb_hit/RING_BUFFER_SIZE;
            g_pmem_metrics[i].TLB_miss[w] = (double)tlb_miss/RING_BUFFER_SIZE;
		}
        g_pmem_metrics[i].stores_count = (double)stores/RING_BUFFER_SIZE;
	}
    print_struc_pmem(g_pmem_tier_ring);
}

void *thread_sample_processor(void *_args){
	struct schedule_manager *args = (struct schedule_manager *) _args;
	int i, w;
    struct timespec start, end;
	
	while(1){
		sleep(SLEEP_TIME);
        
		//This lock is just to read all historical samples
		pthread_mutex_lock(&args->global_mutex);
		
		//alocate the number of total objects, even if exist deallocated 
		g_total_dram_objs = args->tier[0].num_obj;
		g_total_pmem_objs = args->tier[1].num_obj;
        
        //first i copy all date from ring buffer to local variable
		if(g_total_dram_objs > 0 ){
			g_dram_tier_ring = malloc(sizeof(ring_buffer_t)*g_total_dram_objs);
			if(g_dram_tier_ring == NULL){
				fprintf(stderr, "Error during allocation g_dram_tier_ring!!\n");
				exit(-1);
			}
            //Copy variable from shared memory to local variable
			for(i=0; i< g_total_dram_objs; i++){
				g_dram_tier_ring[i] = args->tier[0].obj_vector[i].ring;
			}
        }
		
		if(g_total_pmem_objs > 0 ){
			g_pmem_tier_ring = malloc(sizeof(ring_buffer_t)*g_total_pmem_objs);
			if(g_pmem_tier_ring == NULL){
                fprintf(stderr, "Error during allocation g_pmem_tier_ring!!\n");
				exit(-1);
			}
            //Copy variable from shared memory to local variable
			for(i=0; i< g_total_pmem_objs; i++){
				g_pmem_tier_ring[i] = args->tier[1].obj_vector[i].ring;
			}
		}
		pthread_mutex_unlock(&args->global_mutex);
	
        //now, with shared mamery free, we can calculate the Simple Moving Average(SMA) for each metric
        if(g_total_dram_objs > 0 ){
            g_dram_metrics = malloc(sizeof(metric_t)*g_total_dram_objs);
            if(g_dram_metrics == NULL){
                fprintf(stderr, "Error during allocation g_dram_metrics!!\n");
                exit(-1);
            }
            
            clock_gettime(CLOCK_REALTIME, &start);
            calculate_SMA_for_DRAM();
            clock_gettime(CLOCK_REALTIME, &end);
            
            D fprintf(stderr, "[sample_processor] Obj in DRAM:%d Time of processing:%f seconds\n", g_total_dram_objs, get_timestamp_diff_in_seconds(start, end));
            
            free(g_dram_tier_ring);
            free(g_dram_metrics);
        }
        
        if(g_total_pmem_objs > 0 ){
        	g_pmem_metrics = malloc(sizeof(metric_t)*g_total_pmem_objs);
            if(g_pmem_metrics == NULL){
                fprintf(stderr, "Error during allocation g_pmem_metrics!!\n");
                exit(-1);
            }
            
            clock_gettime(CLOCK_REALTIME, &start);
            calculate_SMA_for_PMEM();
            clock_gettime(CLOCK_REALTIME, &end);
            
            D fprintf(stderr, "[sample_processor] Obj in PMEM:%d Time of processing:%f seconds\n", g_total_pmem_objs, get_timestamp_diff_in_seconds(start, end));
            
            free(g_pmem_tier_ring);
            free(g_pmem_metrics);
        }
        
		
		//Again i get the lock to update the shared memory
		double old_value;
		double curr_value;
		
		if(g_total_dram_objs > 0 ){
			pthread_mutex_lock(&args->global_mutex);
			for(i=0; i< g_total_dram_objs; i++){
				for(w=0; w< MEM_LEVELS; w++){ 
				    //update shared memory
				    old_value = args->tier[0].obj_vector[i].metrics.sum_latency_cost[w];
				    curr_value = g_dram_metrics[i].sum_latency_cost[w];
					args->tier[0].obj_vector[i].metrics.sum_latency_cost[w] = (curr_value * (1-ALPHA)) + (old_value * ALPHA);
					
					old_value = args->tier[0].obj_vector[i].metrics.loads_count[w];
					curr_value = g_dram_metrics[i].loads_count[w];
                    args->tier[0].obj_vector[i].metrics.loads_count[w] = (curr_value * (1-ALPHA)) + (old_value * ALPHA) ;
                    
                    old_value = args->tier[0].obj_vector[i].metrics.TLB_hit[w];
                    curr_value = g_dram_metrics[i].TLB_hit[w];
                    args->tier[0].obj_vector[i].metrics.TLB_hit[w] = (curr_value * (1-ALPHA)) + (old_value * ALPHA) ;
                    
                    old_value = args->tier[0].obj_vector[i].metrics.TLB_miss[w];
                    curr_value = g_dram_metrics[i].TLB_miss[w];
                    args->tier[0].obj_vector[i].metrics.TLB_miss[w] = (curr_value * (1-ALPHA)) + (old_value * ALPHA) ;
				}

				old_value = args->tier[0].obj_vector[i].metrics.stores_count;
				curr_value = g_dram_metrics[i].stores_count;
                args->tier[0].obj_vector[i].metrics.stores_count = (curr_value * (1-ALPHA)) + (old_value * ALPHA) ;
			}
            pthread_mutex_unlock(&args->global_mutex);
		}
		
		
		if(g_total_pmem_objs > 0 ){
			pthread_mutex_lock(&args->global_mutex);
            for(i=0; i< g_total_pmem_objs; i++){
                for(w=0; w< MEM_LEVELS; w++){
                    //update shared memory
                    old_value = args->tier[1].obj_vector[i].metrics.sum_latency_cost[w];
				    curr_value = g_pmem_metrics[i].sum_latency_cost[w];
					args->tier[1].obj_vector[i].metrics.sum_latency_cost[w] = (curr_value * (1-ALPHA)) + (old_value * ALPHA);
					
					old_value = args->tier[1].obj_vector[i].metrics.loads_count[w];
					curr_value = g_pmem_metrics[i].loads_count[w];
                    args->tier[1].obj_vector[i].metrics.loads_count[w] = (curr_value * (1-ALPHA)) + (old_value * ALPHA) ;
                    
                    old_value = args->tier[1].obj_vector[i].metrics.TLB_hit[w];
                    curr_value = g_pmem_metrics[i].TLB_hit[w];
                    args->tier[1].obj_vector[i].metrics.TLB_hit[w] = (curr_value * (1-ALPHA)) + (old_value * ALPHA) ;
                    
                    old_value = args->tier[1].obj_vector[i].metrics.TLB_miss[w];
                    curr_value = g_pmem_metrics[i].TLB_miss[w];
                    args->tier[1].obj_vector[i].metrics.TLB_miss[w] = (curr_value * (1-ALPHA)) + (old_value * ALPHA) ;
                }
                
				old_value = args->tier[1].obj_vector[i].metrics.stores_count;
				curr_value = g_pmem_metrics[i].stores_count;
                args->tier[1].obj_vector[i].metrics.stores_count = (curr_value * (1-ALPHA)) + (old_value * ALPHA) ;            
            }
            pthread_mutex_unlock(&args->global_mutex);
		}
        
	}

}
