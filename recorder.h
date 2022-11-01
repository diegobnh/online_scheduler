#ifndef __RECORDER_H_
#define __RECORDER_H_

#include <stdio.h>
#include <time.h>
#include <pthread.h>
#include <limits.h>

#define MAX_OBJECTS 5000
//#define CHUNK_SIZE 10000003072UL ////10GB aligned in pages of 4kb - The size is in bytes
//#define CHUNK_SIZE 1000001536UL  //1GB
//#define CHUNK_SIZE 1073741824UL //1GB usando potencia de 2
//#define CHUNK_SIZE 2000003072UL //2GB
#define CHUNK_SIZE 4000002048UL  //4GB
#define MEM_LEVELS 5

//#define GB 1e9
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
    double tlb_hit[MEM_LEVELS];
    double tlb_miss[MEM_LEVELS];
}metric_t;

typedef struct object{
    unsigned long start_addr;
    unsigned long end_addr;
    unsigned long size;
    unsigned long pages;
    int sliced;
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


int insert_object(int, unsigned long, unsigned long);
int _insert_object(int, unsigned long, unsigned long, int);
int remove_object(int, unsigned long, unsigned long);
void initialize_recorder();

#endif
