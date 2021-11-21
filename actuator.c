

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
        //if(args->tier[0].obj_vector[i].metrics.loads_count[4] > MINIMUM_LLCM && args->tier[0].obj_flag_alloc[i] == 1){
        if(args->tier[0].obj_flag_alloc[i] == 1){
            
            if(args->tier[0].obj_vector[i].metrics.stores_count != 0){
                fprintf(stderr, "DRAM[%d,%.4lf] = %04.4lf,%.4lf read-write\n", args->tier[0].obj_vector[i].index_id, args->tier[0].obj_vector[i].size/GB, args->tier[0].obj_vector[i].metrics.loads_count[4]/(args->tier[0].obj_vector[i].size/GB), args->tier[0].obj_vector[i].metrics.stores_count);
            }else{
                fprintf(stderr, "DRAM[%d,%.4lf] = %04.4lf read-only\n", args->tier[0].obj_vector[i].index_id, args->tier[0].obj_vector[i].size/GB, args->tier[0].obj_vector[i].metrics.loads_count[4]/(args->tier[0].obj_vector[i].size/GB));
            }
            
        }
        
    }
    fprintf(stderr, "\n");
    for(i=0;i<args->tier[1].num_obj;i++){
        //if(args->tier[1].obj_vector[i].metrics.loads_count[4] > MINIMUM_LLCM && args->tier[1].obj_flag_alloc[i] == 1){
        if(args->tier[1].obj_flag_alloc[i] == 1){
            
            if(args->tier[1].obj_vector[i].metrics.stores_count != 0){
                fprintf(stderr, "PMEM[%d,%06.4lf] = %04.4lf,%.2lf read-write\n", args->tier[1].obj_vector[i].index_id, args->tier[1].obj_vector[i].size/GB, args->tier[1].obj_vector[i].metrics.loads_count[4]/(args->tier[1].obj_vector[i].size/GB), args->tier[1].obj_vector[i].metrics.stores_count);
            }else{
                fprintf(stderr, "PMEM[%d,%06.4lf] = %04.4lf read-only\n", args->tier[1].obj_vector[i].index_id, args->tier[1].obj_vector[i].size/GB, args->tier[1].obj_vector[i].metrics.loads_count[4]/(args->tier[1].obj_vector[i].size/GB));
            }
            
            flag_has_llcm = 1;
        }
    }
    
    
    fprintf(stderr, "\nFree DRAM space:%.2lf(GB), DRAM consumed:%.2lf\n", current_dram_space, current_dram_consumed);
    return flag_has_llcm;
}

//Chamda quando tem espaço na DRAM e objeto hot no pmem
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
    
    //Percorre todos os objetos que possuem um LLCM mínimo e que ainda estão alocados
    //Depois verifica se ele também cabe no espaço atual de memória
    for(i=0;i<args->tier[1].num_obj;i++){
        llcm = args->tier[1].obj_vector[i].metrics.loads_count[4]/(args->tier[1].obj_vector[i].size/GB);
        if(llcm > MINIMUM_LLCM && args->tier[1].obj_flag_alloc[i] == 1){
            if ((args->tier[1].obj_vector[i].size/GB) < current_dram_space){
                
                if(mbind((void *)args->tier[1].obj_vector[i].start_addr,
                         args->tier[1].obj_vector[1].size,
                         MPOL_BIND, &nodemask,
                         64,
                         MPOL_MF_MOVE) == -1)
                {
                    fprintf(stderr,"Cant migrate object!!\n");
                    //exit(-1);
                    fprintf(stderr,"Error:%d\n",errno);
                    perror("Error description");
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
        
    }
    fprintf(stderr, "Num obj promoted:%d\n", num_obj_migrated);
    
}

//Chamda quando não tem espaço na DRAM
int policy_migration_demotion(struct schedule_manager *args){
    int i;
    float current_dram_space;
    unsigned long nodemask;
    int top1_pmem;
    float top1_pmem_llcm;
    float top1_pmem_size;
    int num_obj_migrated=0;
    float curr_llcm;
    float sum_llcm_candidates_demotion = 0;
    int list_obj_index[MAX_OBJECTS];
    int cont;
    int curr_index;
    
    pthread_mutex_lock(&args->global_mutex);
    current_dram_space = (MAXIMUM_DRAM_CAPACITY - args->tier[0].current_memory_consumption)/GB;
    pthread_mutex_unlock(&args->global_mutex);
    
    nodemask = 1<<NODE_0_PMEM;
    
    top1_pmem = -1;
    //First get the top1 from pmem to promotion in the next round
    for(i=0;i<args->tier[1].num_obj;i++){
        if(args->tier[1].obj_vector[i].metrics.loads_count[4] > MINIMUM_LLCM && args->tier[1].obj_flag_alloc[i] == 1){
            top1_pmem_llcm = args->tier[1].obj_vector[i].metrics.loads_count[4]/(args->tier[1].obj_vector[i].size/GB);
            top1_pmem_size = args->tier[1].obj_vector[i].size/GB;
            top1_pmem = i;
            fprintf(stderr, "PMEM candidate index:%d, %.2lf, size:%.2lf\n", args->tier[1].obj_vector[i].index_id, top1_pmem_llcm, top1_pmem_size);
            break;
        }
    }
    
    if(top1_pmem == -1)
        return 0;
    
    list_obj_index[0] = -1;
    cont = 0;
    //Stay in the loop until achieve space necessary to move PMEM top 1 or any DRAM object has more LLCM
    for(i=args->tier[0].num_obj-1; i >= 0; i--){
        curr_llcm = args->tier[0].obj_vector[i].metrics.loads_count[4]/(args->tier[0].obj_vector[i].size/GB);
        if(args->tier[0].obj_flag_alloc[i] == 1){
            sum_llcm_candidates_demotion += curr_llcm;
            top1_pmem_size -= args->tier[0].obj_vector[i].size/GB;
            list_obj_index[cont] = i;
            //fprintf(stderr, "list_obj_index[%d] = %d \t addr: %p, size:%.4lf\n", cont, args->tier[0].obj_vector[i].index_id,\
                        args->tier[0].obj_vector[i].start_addr, args->tier[0].obj_vector[i].size/GB);
            cont++;
            if(top1_pmem_size <= 0){
                break;
            }
        }
    }
    list_obj_index[cont] = -1;//To know where i should stop
    
    for(i=0; list_obj_index[i] !=-1; i++){
        fprintf(stderr, "\tObj to remove from DRAM : %d\n", args->tier[0].obj_vector[list_obj_index[i]].index_id);
    }
    fprintf(stderr,"\tSomatorio dos LLCM:%.2lf, PMEM candidate:%.2lf\n", sum_llcm_candidates_demotion, top1_pmem_llcm);
    
    
    cont = 0;
    if(sum_llcm_candidates_demotion < top1_pmem_llcm){
        while(list_obj_index[cont] != -1){
            curr_index = list_obj_index[cont];
            //fprintf(stderr, "\tTry to migrate obj:%d, size:%.4lf --->", args->tier[0].obj_vector[curr_index].index_id, args->tier[0].obj_vector[curr_index].size/GB);
            fprintf(stderr, "\tTry to migrate obj:%d, addr:%p, size:%ld --->", args->tier[0].obj_vector[curr_index].index_id, args->tier[0].obj_vector[curr_index].start_addr,args->tier[0].obj_vector[curr_index].size);
            if(mbind((void *)args->tier[0].obj_vector[curr_index].start_addr,
                     args->tier[0].obj_vector[curr_index].size,
                     MPOL_BIND, &nodemask,
                     64,
                     MPOL_MF_MOVE) == -1)
            {
                fprintf(stderr," Cant migrate object!!\n");
                fprintf(stderr,"Error:%d\n",errno);
                perror("Error description");
                getchar();
                //exit(-1);
            }else{
                fprintf(stderr," Demoted to PMEM ! \n");
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
            
            cont++;
        }
    }else{
        fprintf(stderr,"Sum of all objs from DRAM has more LLCM (%.2lf) than Top1 from PMEM (%.2lf)!!\n",sum_llcm_candidates_demotion, top1_pmem_llcm);
    }
    
    
    
    //Try to remove N objects from DRAM
    /*
    for(i=args->tier[0].num_obj-1; i >= 0; i--){
        curr_llcm = args->tier[0].obj_vector[i].metrics.loads_count[4]/(args->tier[0].obj_vector[i].size/GB);
        if(curr_llcm > 1 && args->tier[0].obj_flag_alloc[i] == 1){
        //if(args->tier[0].obj_flag_alloc[i] == 1){
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
                fprintf(stderr, "Reduce %.2lf GB in DRAM\n", args->tier[0].obj_vector[i].size/GB);
                fprintf(stderr, "top1_pmem_size before update:%.2lf\n", top1_pmem_size);
                top1_pmem_size -= args->tier[0].obj_vector[i].size/GB;
                fprintf(stderr, "top1_pmem_size after update:%.2lf\n", top1_pmem_size);
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
     */
    fprintf(stderr, "Num obj demoted:%d\n", num_obj_migrated);
    
}

void *thread_actuator(void *_args){
    struct schedule_manager *args = (struct schedule_manager *) _args;
    int i,j;
    int flag_has_llcm;
    float current_dram_space;
    float current_dram_consumed;
    
    while(1){
       sleep(ACTUATOR_INTERVAL);
        
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




