#ifndef __RECORDER_H_
#define __RECORDER_H_

#include <stdio.h>
#include <time.h>
#include <pthread.h>
#include <limits.h>

#define MAX_OBJECTS 5000
#define MEM_LEVELS 5

//#define MAXIMUM_DRAM_CAPACITY  4  //means 4GB //ULONG_MAX
#define GB 1000000000.0

#define MAXIMUM_APPS 1

typedef struct data_bind
{
    unsigned long start_addr;
    unsigned long size;
    unsigned long nodemask_target_node;
    int obj_index;
} data_bind_t;


typedef struct metrics{
    //The metrics could be Simply Moving Average, Weighted Moving Average, Exponential Moving Average...
    double sum_latency_cost[MEM_LEVELS];
    double loads_count[MEM_LEVELS];
    double stores_count;
    double TLB_hit[MEM_LEVELS];
    double TLB_miss[MEM_LEVELS];
}metric_t;

typedef struct object{
    unsigned long start_addr;
    unsigned long end_addr;
    unsigned long size;
    unsigned long pages;
    //int alloc_flag;
    //int status;
    int obj_index; //this value represent the index of the vector where the allocations was saved
    int pid;
    metric_t metrics;
}object_t;

/*
 obj_alloc = 1 and obj_status = -1 -> require bind
 obj_alloc = 1 and obj_status != -1 -> not require bind.
 obj_alloc = 0 requires always obj_status = -1. But always will exist obj_alloc = 0 and obj_status !=0  temporarily,
 because first the intercept will change the obj_alloc and after some time the actuator thread will notice.
 */

typedef struct tier_manager{
    object_t obj_vector[MAX_OBJECTS];
    int obj_alloc[MAX_OBJECTS]; //-1 could means not bind yet or already binded. Depends on the value on obj_status. 0 means desalocated.
    int obj_status[MAX_OBJECTS]; //-1 means unmapped by our software control, 0 means DRAM, 2 means PMEM, 
    int pids_to_manager[MAXIMUM_APPS];
    //pthread_mutex_t global_mutex; //mutex for account_shared_library_instances variable
    //pthread_mutexattr_t global_attr_mutex; //mutex for account_shared_library_instances variable
}tier_manager_t;


int insert_object(int pid, unsigned long start_addr, unsigned long size);
int remove_object(int pid, unsigned long start_addr, unsigned long size);
void initialize_recorder();

#endif
