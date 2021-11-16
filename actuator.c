

#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <stdint.h>
#include <numaif.h>
#include <errno.h>
#include "actuator.h"
#include "monitor.h"
#include "recorder.h"

#define SLEEP_TIME 3

#define NODE_0_DRAM 0
#define NODE_0_PMEM 2
#define NODE_1_DRAM 1
#define NODE_1_PMEM 3

//#define DEBUG
#ifdef DEBUG
  #define D if(1)
#else
  #define D if(0)
#endif

#define NUM_CANDIDATES 5

//float g_top_to_demotion[NUM_CANDIDATES];
//float g_top_to_promotion[NUM_CANDIDATES];
typedef struct candidates{
    int index;
    float llcm;
}candidates_t;

void sort_objects(struct schedule_manager *args){
    int i,j;
    object_t aux;
    
    candidates_t dram_list[MAX_OBJECTS];//alocação dinamica aqui não funciona! Trava!
    candidates_t pmem_list[MAX_OBJECTS];//

    for(i=0;i<args->tier[0].num_obj;i++){
        for(j=i+1;j<args->tier[0].num_obj;j++){
            if(args->tier[0].obj_vector[i].metrics.loads_count[4]/(args->tier[0].obj_vector[i].size/1000000000.0) < args->tier[0].obj_vector[j].metrics.loads_count[4]/(args->tier[0].obj_vector[j].size/1000000000.0)){
                aux = args->tier[0].obj_vector[j];
                args->tier[0].obj_vector[j] = args->tier[0].obj_vector[i];
                args->tier[0].obj_vector[i] = aux;
            }
        }
       
    }
    for(i=0;i<args->tier[1].num_obj;i++){
        for(j=i+1;j<args->tier[1].num_obj;j++){
            if(args->tier[1].obj_vector[i].metrics.loads_count[4]/(args->tier[1].obj_vector[i].size/1000000000.0) < args->tier[1].obj_vector[j].metrics.loads_count[4]/(args->tier[1].obj_vector[j].size/1000000000.0)){
                aux = args->tier[1].obj_vector[j];
                args->tier[1].obj_vector[j] = args->tier[1].obj_vector[i];
                args->tier[1].obj_vector[i] = aux;
            }
        }
    }
    
}

int check_candidates_to_migration(struct schedule_manager *args){
    int i;
    int j;
    int flag_has_llcm = 0;
    float current_dram_space;
    
    current_dram_space = (MAXIMUM_DRAM_CAPACITY - args->tier[0].current_memory_consumption)/1000000000.0;
    //current_dram_space = (args->tier[0].current_memory_consumption/1000000000.0);
    if(current_dram_space < 0){
        current_dram_space = 0;
    }
    
    for(i=0;i<args->tier[0].num_obj;i++){
        if(args->tier[0].obj_vector[i].metrics.loads_count[4] != 0 && args->tier[0].obj_flag_alloc[i] == 1){
            if(args->tier[0].obj_vector[i].metrics.stores_count != 0){
                fprintf(stderr, "DRAM[%d,%.4lf] = %04.2lf,%.2lf read-write\n", i, args->tier[0].obj_vector[i].size/1000000000.0, args->tier[0].obj_vector[i].metrics.loads_count[4]/(args->tier[0].obj_vector[i].size/1000000000.0), args->tier[0].obj_vector[i].metrics.stores_count);
            }else{
                fprintf(stderr, "DRAM[%d,%.4lf] = %04.2lf read-only\n", i, args->tier[0].obj_vector[i].size/1000000000.0, args->tier[0].obj_vector[i].metrics.loads_count[4]/(args->tier[0].obj_vector[i].size/1000000000.0));
            }
            flag_has_llcm = 1;
        }
        
    }
    fprintf(stderr, "\n");
    for(i=0;i<args->tier[1].num_obj;i++){
        if(args->tier[1].obj_vector[i].metrics.loads_count[4] != 0 && args->tier[1].obj_flag_alloc[i] == 1){
            if(args->tier[1].obj_vector[i].metrics.stores_count != 0){
                fprintf(stderr, "PMEM[%d,%06.4lf] = %04.2lf,%.2lf read-write\n", i, args->tier[1].obj_vector[i].size/1000000000.0, args->tier[1].obj_vector[i].metrics.loads_count[4]/(args->tier[1].obj_vector[i].size/1000000000.0), args->tier[1].obj_vector[i].metrics.stores_count);
            }else{
                fprintf(stderr, "PMEM[%d,%06.4lf] = %04.2lf read-only\n", i, args->tier[1].obj_vector[i].size/1000000000.0, args->tier[1].obj_vector[i].metrics.loads_count[4]/(args->tier[1].obj_vector[i].size/1000000000.0));
            }
            flag_has_llcm = 1;
        }
    }
    
    //fprintf(stderr, "\nMAXIMUM_DRAM_CAPACITY:%ld", MAXIMUM_DRAM_CAPACITY);
    //fprintf(stderr, "\nCurrent DRAM consumption:%ld", args->tier[0].current_memory_consumption);
    //fprintf(stderr, "\nCurrent PMEM consumption:%ld", args->tier[1].current_memory_consumption);
    fprintf(stderr, "\nCurrent DRAM space:%.4lf(GB), %.2lf\n", current_dram_space, args->tier[0].current_memory_consumption/1000000000.0);
    return flag_has_llcm;
}

void policy_migration_upgrade(struct schedule_manager *args){
    int i;
    float current_dram_space;
    unsigned long nodemask;
    int num_obj_migrated=0;
    
    current_dram_space = (MAXIMUM_DRAM_CAPACITY - args->tier[0].current_memory_consumption)/1000000000.0;
    
    nodemask = 1<<NODE_0_DRAM;
    
    for(i=0;i<args->tier[1].num_obj;i++){
        if ((args->tier[1].obj_vector[i].size/1000000000.0) < current_dram_space){
            
            if(mbind((void *)args->tier[1].obj_vector[i].start_addr,
                     args->tier[1].obj_vector[1].size,
                     MPOL_BIND, &nodemask,
                     64,
                     MPOL_MF_MOVE) == -1)
            {
                //fprintf(stderr,"Cant migrate object!!\n");
                //exit(-1);
            }else{
                num_obj_migrated++;
                remove_allocation_on_pmem(args,
                                      args->tier[1].obj_vector[i].pid,
                                      args->tier[1].obj_vector[i].start_addr,
                                      args->tier[1].obj_vector[i].size);
                
                insert_allocation_on_dram(args,
                                      args->tier[1].obj_vector[i].pid,
                                      args->tier[1].obj_vector[i].start_addr,
                                      args->tier[1].obj_vector[i].size);
                
                current_dram_space += args->tier[1].obj_vector[i].size/1000000000.0;
                
            }
            
        }
    }
    fprintf(stderr, "\nNum obj upgraded:%d\n", num_obj_migrated);
    
}
int policy_migration_downgrade(struct schedule_manager *args){
    int i;
    float current_dram_space;
    unsigned long nodemask;
    int top1_pmem = -1;
    float top1_pmem_llcm;
    float top1_pmem_size;
    int num_obj_migrated=0;
    
    current_dram_space = (MAXIMUM_DRAM_CAPACITY - args->tier[0].current_memory_consumption)/1000000000.0;
    nodemask = 1<<NODE_0_PMEM;
    
    //First get the top1 from pmem to upgrade in the next round
    for(i=0;i<args->tier[1].num_obj;i++){
        if(args->tier[1].obj_vector[i].metrics.loads_count[4] != 0 && args->tier[1].obj_flag_alloc[i] == 1){
            top1_pmem_llcm = args->tier[1].obj_vector[i].metrics.loads_count[4];
            top1_pmem_size = args->tier[1].obj_vector[i].size/1000000000.0;
            top1_pmem = i;
        }
    }
    
    if(top1_pmem == -1)
        return 0;
    
    //Try to remove N objects from DRAM
    for(i=args->tier[0].num_obj-1;i>=0;i--){
        if(args->tier[0].obj_vector[i].metrics.loads_count[4] != 0 && args->tier[0].obj_flag_alloc[i] == 1){
            if(args->tier[0].obj_vector[i].metrics.loads_count[4] < top1_pmem_llcm){
                
                if(mbind((void *)args->tier[0].obj_vector[i].start_addr,
                         args->tier[0].obj_vector[1].size,
                         MPOL_BIND, &nodemask,
                         64,
                         MPOL_MF_MOVE) == -1)
                {
                    //fprintf(stderr,"Cant migrate object!!\n");
                    //exit(-1);
                }else{
                    num_obj_migrated++;
                    remove_allocation_on_dram(args,
                                          args->tier[0].obj_vector[i].pid,
                                          args->tier[0].obj_vector[i].start_addr,
                                          args->tier[0].obj_vector[i].size);
                    
                    insert_allocation_on_pmem(args,
                                          args->tier[0].obj_vector[i].pid,
                                          args->tier[0].obj_vector[i].start_addr,
                                          args->tier[0].obj_vector[i].size);
                }
                
                top1_pmem_size -= args->tier[0].obj_vector[i].size/1000000000.0;
                if(top1_pmem_size < 0){
                    break;
                }
            }
        }
        
    }
    fprintf(stderr, "\nNum obj downgraded:%d\n", num_obj_migrated);
    fprintf(stderr, "-------------------------------------\n");
}

void *thread_actuator(void *_args){
    struct schedule_manager *args = (struct schedule_manager *) _args;
    int i,j;
    int flag_has_llcm;
    
    while(1){
       sleep(SLEEP_TIME);
        
       int random_index;
       unsigned long nodemask = 1<<NODE_1_DRAM;
       
       pthread_mutex_lock(&args->global_mutex);

       sort_objects(args);
       flag_has_llcm = check_candidates_to_migration(args);
         
       pthread_mutex_unlock(&args->global_mutex);

       if(flag_has_llcm == 1)  {
           policy_migration_upgrade(args);
           policy_migration_downgrade(args);
       }
        
       
       
    }//while end
}




