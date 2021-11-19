

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

#define MINIMUM_LLCM 1

//float g_top_to_demotion[NUM_CANDIDATES];
//float g_top_to_promotion[NUM_CANDIDATES];
typedef struct candidates{
    int index;
    float llcm;
}candidates_t;

void sort_objects(struct schedule_manager *args){
    int i,j;
    object_t aux;

    for(i=0;i<args->tier[0].num_obj;i++){
        for(j=i+1;j<args->tier[0].num_obj;j++){
            if(args->tier[0].obj_vector[i].metrics.loads_count[4]/(args->tier[0].obj_vector[i].size/GB) < args->tier[0].obj_vector[j].metrics.loads_count[4]/(args->tier[0].obj_vector[j].size/GB))
            {
                aux = args->tier[0].obj_vector[j];
                args->tier[0].obj_vector[j] = args->tier[0].obj_vector[i];
                args->tier[0].obj_vector[i] = aux;
            }
        }
       
    }
    for(i=0;i<args->tier[1].num_obj;i++){
        for(j=i+1;j<args->tier[1].num_obj;j++){
            if(args->tier[1].obj_vector[i].metrics.loads_count[4]/(args->tier[1].obj_vector[i].size/GB) < args->tier[1].obj_vector[j].metrics.loads_count[4]/(args->tier[1].obj_vector[j].size/GB))
            {
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
    float current_dram_consumed;
    
    current_dram_space = (MAXIMUM_DRAM_CAPACITY - args->tier[0].current_memory_consumption)/GB;
    current_dram_consumed = args->tier[0].current_memory_consumption/GB;
    
    if(current_dram_space < 0){
        current_dram_space = 0;
    }
    
    fprintf(stderr, "Context\n");
    for(i=0;i<args->tier[0].num_obj;i++){
        if(args->tier[0].obj_vector[i].metrics.loads_count[4] >= MINIMUM_LLCM && args->tier[0].obj_flag_alloc[i] == 1){
            
            if(args->tier[0].obj_vector[i].metrics.stores_count != 0){
                fprintf(stderr, "DRAM[%d,%.4lf] = %04.4lf,%.4lf read-write\n", args->tier[0].obj_vector[i].index_id, args->tier[0].obj_vector[i].size/GB, args->tier[0].obj_vector[i].metrics.loads_count[4]/(args->tier[0].obj_vector[i].size/GB), args->tier[0].obj_vector[i].metrics.stores_count);
            }else{
                fprintf(stderr, "DRAM[%d,%.4lf] = %04.4lf read-only\n", args->tier[0].obj_vector[i].index_id, args->tier[0].obj_vector[i].size/GB, args->tier[0].obj_vector[i].metrics.loads_count[4]/(args->tier[0].obj_vector[i].size/GB));
            }
            
        }
        
    }
    fprintf(stderr, "\n");
    for(i=0;i<args->tier[1].num_obj;i++){
        if(args->tier[1].obj_vector[i].metrics.loads_count[4] > MINIMUM_LLCM && args->tier[1].obj_flag_alloc[i] == 1){
            
            if(args->tier[1].obj_vector[i].metrics.stores_count != 0){
                fprintf(stderr, "PMEM[%d,%06.4lf] = %04.4lf,%.4lf read-write\n", args->tier[1].obj_vector[i].index_id, args->tier[1].obj_vector[i].size/GB, args->tier[1].obj_vector[i].metrics.loads_count[4]/(args->tier[1].obj_vector[i].size/GB), args->tier[1].obj_vector[i].metrics.stores_count);
            }else{
                fprintf(stderr, "PMEM[%d,%06.4lf] = %04.4lf read-only\n", args->tier[1].obj_vector[i].index_id, args->tier[1].obj_vector[i].size/GB, args->tier[1].obj_vector[i].metrics.loads_count[4]/(args->tier[1].obj_vector[i].size/GB));
            }
            
            flag_has_llcm = 1;
        }
    }
    
    
    fprintf(stderr, "Current DRAM space:%.2lf(GB), DRAM consumed:%.2lf\n", current_dram_space, current_dram_consumed);
    return flag_has_llcm;
}
void policy_migration_promotion(struct schedule_manager *args){
    int i;
    float current_dram_space;
    unsigned long nodemask;
    int num_obj_migrated=0;
    float llcm;
    
    pthread_mutex_lock(&args->global_mutex);
    current_dram_space = (MAXIMUM_DRAM_CAPACITY - args->tier[0].current_memory_consumption)/GB;
    pthread_mutex_unlock(&args->global_mutex);

    nodemask = 1<<NODE_0_DRAM;
    
    
    for(i=0;i<args->tier[1].num_obj;i++){
        llcm = args->tier[1].obj_vector[i].metrics.loads_count[4]/(args->tier[1].obj_vector[i].size/GB);
        //if(args->tier[1].obj_flag_alloc[i] == 1)
        //   fprintf(stderr, "Checking if Hottest PMEM size (%.2lf) <  Current space in DRAM (%ld)\n", llcm,current_dram_space);
        if ((args->tier[1].obj_vector[i].size/GB) < current_dram_space && args->tier[1].obj_flag_alloc[i] == 1){
            
            if(mbind((void *)args->tier[1].obj_vector[i].start_addr,
                     args->tier[1].obj_vector[1].size,
                     MPOL_BIND, &nodemask,
                     64,
                     MPOL_MF_MOVE) == -1)
            {
                //fprintf(stderr,"Cant migrate object!!\n");
                //exit(-1);
            }else{
                fprintf(stderr,"Promoted to DRAM object:%d \n", args->tier[1].obj_vector[i].index_id);
                num_obj_migrated++;
                remove_allocation_on_pmem(args,
                                      args->tier[1].obj_vector[i].pid,
                                      args->tier[1].obj_vector[i].start_addr,
                                      args->tier[1].obj_vector[i].size);
                
                insert_allocation_on_dram(args,
                                      args->tier[1].obj_vector[i].pid,
                                      args->tier[1].obj_vector[i].start_addr,
                                      args->tier[1].obj_vector[i].size);
                
                current_dram_space += args->tier[1].obj_vector[i].size/GB;
                
            }
        }else{
            if(args->tier[1].obj_flag_alloc[i] == 1){
                fprintf(stderr,"Cannot promote object %d to DRAM because of size:%lf \n", \
                        args->tier[1].obj_vector[i].index_id,\
                        args->tier[1].obj_vector[i].size/GB);
            }else{
                fprintf(stderr,"Cannot promote object %d to DRAM because its desalocatted!\n",args->tier[1].obj_vector[i].index_id);
            }
            
        }
    }
    fprintf(stderr, "Num obj promoted:%d\n", num_obj_migrated);
    
}
int policy_migration_demotion(struct schedule_manager *args){
    int i;
    float current_dram_space;
    unsigned long nodemask;
    int top1_pmem = -1;
    float top1_pmem_llcm;
    float top1_pmem_size;
    int num_obj_migrated=0;
    float curr_llcm;
    float sum_llcm_candidates_demotion = 0;
    int obj_index_to_demotion[MAX_OBJECTS];
    int index_demotion=0;
    int curr_index;
    
    pthread_mutex_lock(&args->global_mutex);
    current_dram_space = (MAXIMUM_DRAM_CAPACITY - args->tier[0].current_memory_consumption)/GB;
    pthread_mutex_unlock(&args->global_mutex);
    
    nodemask = 1<<NODE_0_PMEM;
    
    //First get the top1 from pmem to promotion in the next round - get candidate
    for(i=0;i<args->tier[1].num_obj;i++){
        if(args->tier[1].obj_vector[i].metrics.loads_count[4] > MINIMUM_LLCM && args->tier[1].obj_flag_alloc[i] == 1){
            top1_pmem_llcm = args->tier[1].obj_vector[i].metrics.loads_count[4]/(args->tier[1].obj_vector[i].size/GB);
            top1_pmem_size = args->tier[1].obj_vector[i].size/GB;
            top1_pmem = i;
            fprintf(stderr, "PMEM candidate index:%d, %.2lf\n", i, top1_pmem_llcm);
            break;
        }
    }
    
    if(top1_pmem == -1)
        return 0;
    
    obj_index_to_demotion[0] = -1;
    //Stay in the loop until achieve space necessary to move PMEM top 1 or any DRAM object has more LLCM
    for(i=args->tier[0].num_obj-1; i >= 0; i--){
        curr_llcm = args->tier[0].obj_vector[i].metrics.loads_count[4]/(args->tier[0].obj_vector[i].size/GB);
        if(curr_llcm > MINIMUM_LLCM && args->tier[0].obj_flag_alloc[i] == 1){
            if(curr_llcm < top1_pmem_llcm){
                sum_llcm_candidates_demotion += curr_llcm;
                top1_pmem_size -= args->tier[0].obj_vector[i].size/GB;
                obj_index_to_demotion[index_demotion] = i;
                index_demotion++;
                if(top1_pmem_size <= 0){
                    break;
                }
            }else{
                break;
            }
        }
    }
    obj_index_to_demotion[index_demotion] = -1;//To know where i should stop
    
    
    index_demotion = 0;
    if(sum_llcm_candidates_demotion < top1_pmem_llcm){
        while(obj_index_to_demotion[index_demotion] != -1){
            curr_index = obj_index_to_demotion[index_demotion];
            
            if(mbind((void *)args->tier[0].obj_vector[curr_index].start_addr,
                     args->tier[0].obj_vector[curr_index].size,
                     MPOL_BIND, &nodemask,
                     64,
                     MPOL_MF_MOVE) == -1)
            {
                fprintf(stderr,"Cant migrate object!!\n");
                //exit(-1);
            }else{
                fprintf(stderr,"Demoted to PMEM object:%d \n", args->tier[0].obj_vector[curr_index].index_id);
                num_obj_migrated++;
                remove_allocation_on_dram(args,
                                      args->tier[0].obj_vector[curr_index].pid,
                                      args->tier[0].obj_vector[curr_index].start_addr,
                                      args->tier[0].obj_vector[curr_index].size);
                
                insert_allocation_on_pmem(args,
                                      args->tier[0].obj_vector[curr_index].pid,
                                      args->tier[0].obj_vector[curr_index].start_addr,
                                      args->tier[0].obj_vector[curr_index].size);
            }
            
            index_demotion++;
        }
    }else{
        fprintf(stderr,"Sum of all objs from DRAM has more LLCM than Top1 form PMEM!!\n");
    }
    
    
    //Try to remove N objects from DRAM
    /*
    for(i=args->tier[0].num_obj-1; i >= 0; i--){
        curr_llcm = args->tier[0].obj_vector[i].metrics.loads_count[4]/(args->tier[0].obj_vector[i].size/GB);
        if(curr_llcm > 1 && args->tier[0].obj_flag_alloc[i] == 1){
            fprintf(stderr, "Checking if Coldest DRAM (%.2lf) <  Hottest PMEM (%.2lf)\n", curr_llcm,top1_pmem_llcm);
            if(curr_llcm < top1_pmem_llcm){
                
                if(mbind((void *)args->tier[0].obj_vector[i].start_addr,
                         args->tier[0].obj_vector[i].size,
                         MPOL_BIND, &nodemask,
                         64,
                         MPOL_MF_MOVE) == -1)
                {
                    fprintf(stderr,"Cant migrate object!!\n");
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
                
                top1_pmem_size -= args->tier[0].obj_vector[i].size/GB;
                if(top1_pmem_size <= 0){
                    fprintf(stderr, "Espaço liberado na DRAM suficiente para migrar Top1 do PMEM\n");
                    break;
                }
            }else{
                fprintf(stderr, "Object %d in DRAM has more LLCM/GB:%.4lf\n", i, curr_llcm);
                break;
            }
        }
    }
    fprintf(stderr, "Num obj demoted:%d\n", num_obj_migrated);
    */
}

void *thread_actuator(void *_args){
    struct schedule_manager *args = (struct schedule_manager *) _args;
    int i,j;
    int flag_has_llcm;
    float current_dram_space;
    float current_dram_consumed;
    
    while(1){
       sleep(SLEEP_TIME);
        
       int random_index;
       unsigned long nodemask = 1<<NODE_1_DRAM;
       
       pthread_mutex_lock(&args->global_mutex);
        
       sort_objects(args);
       current_dram_space = (MAXIMUM_DRAM_CAPACITY - args->tier[0].current_memory_consumption)/GB;
       current_dram_consumed = args->tier[0].current_memory_consumption/GB;
       flag_has_llcm = check_candidates_to_migration(args);
        
       pthread_mutex_unlock(&args->global_mutex);

       fprintf(stderr, "\nDecisions\n");
       if(flag_has_llcm == 1 && current_dram_space > 0)  {
           policy_migration_promotion(args);//move top objects from PMEM to DRAM
       }
       else if(current_dram_space <= 0){
           policy_migration_demotion(args);//move non-top objetcts from DRAM to PMEM
       }
       fprintf(stderr, "-------------------------------------------------------\n");
    }//while end
}




