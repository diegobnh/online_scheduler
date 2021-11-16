#ifndef __RECORDER_H_
#define __RECORDER_H_

#include <stdio.h>
#include <time.h>
#include <pthread.h>

#define MAX_OBJECTS 1000
#define RING_BUFFER_SIZE  3
#define MEM_LEVELS 5
#define MAXIMUM_DRAM_CAPACITY 4E+9  //means 4GB

typedef struct metrics{
    //The metrics could be Simply Moving Average, Weighted Moving Average, Exponential Moving Average...
    double sum_latency_cost[MEM_LEVELS];
    double loads_count[MEM_LEVELS];
    double stores_count;
    double TLB_hit[MEM_LEVELS];
    double TLB_miss[MEM_LEVELS];
}metric_t;

typedef struct ring_buffer {
    int allocation_index;
    int current_ring_index;
    int node_allocation; //we assume 0 to DRAM and 1 to PMEM
    unsigned long sum_latency_cost[RING_BUFFER_SIZE][MEM_LEVELS];
    unsigned long loads_count[RING_BUFFER_SIZE][MEM_LEVELS]; //We split to several mem level: L0, LFB, L1, L2, L3, DRAM
    unsigned long stores_count[RING_BUFFER_SIZE]; //Limited to L1
    unsigned long TLB_hit[RING_BUFFER_SIZE][MEM_LEVELS];
    unsigned long TLB_miss[RING_BUFFER_SIZE][MEM_LEVELS];
}ring_buffer_t;


typedef struct object{
    unsigned long start_addr;
    unsigned long end_addr;
    unsigned long size;
    unsigned long pages;
    int index_id; //this value represent the index of the vector where the allocations was saved
    int pid;
    ring_buffer_t ring;
    metric_t metrics;
}object_t;

typedef struct tier_manager{
    object_t obj_vector[MAX_OBJECTS];
    int obj_flag_alloc[MAX_OBJECTS];
    int num_obj;
    unsigned long current_memory_consumption;
    int current_obj_index; //this variable controls the next position free to allocate
}tier_manager_t;

struct schedule_manager{
    tier_manager_t tier[2];//tier[0] means DRAM and tier[1] means PMEM
    pthread_mutex_t global_mutex; //mutex for account_shared_library_instances variable
    pthread_mutexattr_t global_attr_mutex; //mutex for account_shared_library_instances variable
    int account_shared_library_instances; //control variable to know when to deallocate the shared region
};

object_t * allocate_object(void);
int insert_allocation_on_dram(struct schedule_manager *, int pid, unsigned long start_addr, unsigned long size);
int insert_allocation_on_pmem(struct schedule_manager *, int pid, unsigned long start_addr, unsigned long size);
int remove_allocation_on_dram(struct schedule_manager *, int pid, unsigned long start_addr, unsigned long size);
int remove_allocation_on_pmem(struct schedule_manager *, int pid, unsigned long start_addr, unsigned long size);
void initialize_recorder(struct schedule_manager *);

FILE * open_pagemap_file(void);
FILE * open_kpage_file(void);
#endif
