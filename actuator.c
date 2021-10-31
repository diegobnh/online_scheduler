

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
#define NODE_1_DRAM 1
#define NODE_2_PMEM 2
#define NODE_3_PMEM 3


#define DEBUG
#ifdef DEBUG
  #define D if(1)
#else
  #define D if(0)
#endif

void *thread_actuator(void *_args){
    struct schedule_manager *args = (struct schedule_manager *) _args;
    int i,j;
    
    while(1){
       sleep(10);
        
       int random_index;
       unsigned long nodemask = 1<<NODE_1_DRAM;

       pthread_mutex_lock(&args->global_mutex);
       /*	
       for(i=0 ;i< args->tier[0].num_obj; i++){
             if(args->tier[0].obj_vector[i].metrics.loads_count[4] != 0 && \
                args->tier[0].obj_flag_alloc[i] == 1)//has LLCM and is an active allocation
             {
                   fprintf(stderr, "DRAM - index:%d, LLC miss:%.2lf \n", i, args->tier[0].obj_vector[i].metrics.loads_count[4]);
             }
       }
       
       for(i=0 ;i< args->tier[1].num_obj; i++){
             if(args->tier[1].obj_vector[i].metrics.loads_count[4] != 0 && \
                args->tier[1].obj_flag_alloc[i] == 1)//has LLCM and is an active allocation
             {
                   fprintf(stderr, "PMEM - index:%d, LLC miss:%.2lf \n", i, args->tier[1].obj_vector[i].metrics.loads_count[4]);
             }
       }
       fprintf(stderr, "----------------------------\n");
       pthread_mutex_unlock(&args->global_mutex);
       */
       

       
       do{
           random_index = (rand() % (args->tier[0].num_obj + 1));
       }while(args->tier[0].obj_flag_alloc[random_index] == 0);
       
       D fprintf(stderr, "Position obj:%d, Moving addr:%p size:%lu from DRAM to PMEM\n", random_index, args->tier[0].obj_vector[random_index].start_addr, args->tier[0].obj_vector[random_index].size);
                    
       if(mbind((void *)args->tier[0].obj_vector[random_index].start_addr,
                args->tier[0].obj_vector[random_index].size,
                MPOL_BIND, &nodemask,
                64,
                MPOL_MF_MOVE) == -1)
       {
           fprintf(stderr,"Cant migrate object: %s\n",perror());
           //exit(-1);
       }else{
         	remove_allocation_on_dram(args,
                                 args->tier[0].obj_vector[random_index].pid,
                                 args->tier[0].obj_vector[random_index].start_addr,
                                 args->tier[0].obj_vector[random_index].size);
            insert_allocation_on_pmem(args,
                                 args->tier[0].obj_vector[random_index].pid,
                                 args->tier[0].obj_vector[random_index].start_addr,
                                 args->tier[0].obj_vector[random_index].size);
       }
       
       
      pthread_mutex_unlock(&args->global_mutex);
    
    }//while end
}




