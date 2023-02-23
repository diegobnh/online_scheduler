#ifndef __RECORDER_H_
#define __RECORDER_H_

#include <stdio.h>
#include <time.h>
#include <pthread.h>
#include <limits.h>

#define MAX_OBJECTS 2000
#define CHUNK_SIZE 1000001536UL  //1GB
//#define CHUNK_SIZE 500002816UL //500MB
#define MEM_LEVELS 5 //l1, lfb, l2, l3, dram

//#define GB 1.0e9
#define GB 1000000000.0
#define MAXIMUM_APPS 1

#define NODE_0_DRAM 0
#define NODE_0_PMEM 2
#define NODE_1_DRAM 1
#define NODE_1_PMEM 3

#define ROUND_ROBIN 1
#define RANDOM 2
#define FIRST_DRAM 3
#define BASED_ON_SIZE 4


#define DEBUG

/* Flags for mbind FROM mempolicy.h*/
//#define MPOL_MF_STRICT    (1<<0)    /* Verify existing pages in the mapping */
//#define MPOL_MF_MOVE     (1<<1)    /* Move pages owned by this process to conform*/

typedef enum bind_type {
    INITIAL_DATAPLACEMENT = 1,
    PROMOTION = 2,
    DEMOTION = 3
} bind_type_t;

typedef struct data_bind
{
    unsigned long start_addr;
    unsigned long size;
    unsigned long nodemask_target_node;
    int obj_index;
    bind_type_t type;
    int flag;
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
    double birth_date;//could be when the object/chunk strart or when moved to another type of memory
    float page_status[4];//4 means 3 nodes + 1
    int sliced;//If 1, means this is a chunk. If 0, means the size is less than chunk size
    int obj_index; //This value represent the index of the vector where the allocations was saved
    int pid;
    int last_decision;
    metric_t metrics;
}object_t;

/*
 obj_alloc = 0 and obj_status == -1 -> Not exist
 obj_alloc = 1 and obj_status == -1 -> require bind
 obj_alloc = 1 and obj_status != -1 -> not require bind.
 */

typedef struct tier_manager{
    object_t obj_vector[MAX_OBJECTS];
    int obj_alloc[MAX_OBJECTS]; //-1 could means not bind yet or already binded. Depends on the value on obj_status. 0 means desalocated.
    int pids_to_manager[MAXIMUM_APPS];
    int total_obj;
    float total_dram_mapped_gb;
    //pthread_mutex_t global_mutex; //mutex for account_shared_library_instances variable
    //pthread_mutexattr_t global_attr_mutex; //mutex for account_shared_library_instances variable
}tier_manager_t;


int insert_object(int, unsigned long, unsigned long);
int _insert_object(int, unsigned long, unsigned long, int);
int deallocate_object(int, unsigned long, unsigned long);
int _deallocate_object(int, unsigned long, unsigned long, int);
void initialize_recorder();
void recorder_open_pipes();
int initial_dataplacement_policy(unsigned long , unsigned long , int , int );
#endif
