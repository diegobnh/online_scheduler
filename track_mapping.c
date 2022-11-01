#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <stdint.h>
#include <numaif.h>
#include <numa.h>
#include <errno.h>
#include <signal.h>
#include <dlfcn.h>
#include <sys/ipc.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <assert.h>

#include "recorder.h"

#define DEBUG
#ifdef DEBUG
  #define D if(1)
#else
  #define D if(0)
#endif

#define METRIC_ABS_LLCM 1
#define METRIC_LLCM_PER_SIZE 2
#define METRIC_ABS_TLB_MISS 3
#define METRIC_TLB_MISS_PER_SIZE 4
#define METRIC_ABS_WRITE 5
#define METRIC_WRITE_PER_SIZE 6
#define METRIC_ABS_LATENCY 7
#define METRIC_LATENCY_PER_SIZE 8

#ifndef METRIC
#error "METRIC is not defined."
#endif

#if !(METRIC >= 1 && METRIC <= 8)
#error "METRIC value invalid."
#endif


FILE *g_fp;
extern tier_manager_t g_tier_manager;
extern volatile sig_atomic_t g_running;
extern float g_track_mapping_interval;
static int g_num_nodes_available;
extern int g_hotness_threshold;//When this value is very low,we have errors in numa_move_pages()

struct key_value{
    int key;
    double value;
};

static struct key_value g_key_value;
static struct key_value g_sorted_obj[MAX_OBJECTS];

void query_status_memory_pages(int pid, unsigned long int addr, unsigned long int size, float *status_memory_pages){
    /*
     query_status_memory_pages uses the move_pages syscall in query mode (with null nodes) to check the page's status
     of pages in a specifc interval.
     
     query_status_memory_pages returns the id of the NUMA node of the pointer in status.
    */
    unsigned node = 0;
    // size must be page-aligned
    size_t pagesize = getpagesize();
    assert((size % pagesize) == 0);

    unsigned long page_count = size / pagesize;
    void **pages_addr;
    int *status;
    int *nodes;
    
    pages_addr = malloc(sizeof(char *) * page_count);
    status = malloc(sizeof(int *) * page_count);
    nodes = malloc(sizeof(int *) * page_count);

    if (!pages_addr || !status || !nodes) {
       fprintf(stderr, "Unable to allocate memory\n");
       exit(1);
    }

    for (int i = 0; i < page_count; i++) {
        pages_addr[i] = (void *) addr + i * pagesize;
        nodes[i] = node;
        status[i] = -1;
    }

    if (numa_move_pages(pid, page_count, pages_addr, NULL, status, MPOL_MF_MOVE) == -1) {
        //fprintf(stderr, "[track_mapping - numa_move_pages] error code: %d ", errno);
        //perror(" ");
        for(int i = 0; i < g_num_nodes_available + 1; i++){
            status_memory_pages[i] = -1;
        }
    }else{
        for (int i = 0; i < page_count; i++) {
            if(status[i] >= 0 && status[i] < g_num_nodes_available){
                status_memory_pages[status[i]]++;//0,1,2,3
            }else{
                status_memory_pages[g_num_nodes_available]++;
            }
        }
        
        for(int i = 0; i < g_num_nodes_available + 1; i++){
            status_memory_pages[i] = (float)status_memory_pages[i]/page_count;
        }
    }
}
void set_number_of_nodes_availables(void){
    if ((numa_available() < 0)) {
        fprintf(stderr, "error: not a numa machine\n");
        exit(-1);
    }
    
    unsigned node = 0;

    int num_nodes = numa_max_node() + 1;
    if (num_nodes < 2) {
        fprintf(stderr, "error: a minimum of 2 nodes is required\n");
        exit(-1);
    }
    
    g_num_nodes_available = num_nodes;
}
int open_output_file(void){
    char filename[50];
    
    sprintf(filename, "track_mapping.txt");
    //sprintf(filename, "/tmp/track_mapping.txt");
    g_fp = fopen(filename, "w");
    if(g_fp == NULL){
        fprintf(stderr, "[track_mapping] Error while trying to open track decisions\n");
        perror(filename);
        return -1;
    }else{
        fprintf(g_fp, "interval,timestamp,index,start_addr,size,metric_value,dram_0,dram_1,pmem_2,pmem_3,not_allocated\n");
        return 0;
    }
}
static int comp(const void * elem1, const void * elem2){
    struct key_value *k_v1, *k_v2;
    
    k_v1 = (struct key_value*)elem1;
    k_v2 = (struct key_value*)elem2;
    
    double f = k_v1->value;
    double s = k_v2->value;
    
    if (f < s) return  1;
    if (f > s) return -1;
    return 0;
}
static double get_hotness_metric(int obj_index){
    
#if METRIC == METRIC_ABS_LLCM
    return g_tier_manager.obj_vector[obj_index].metrics.loads_count[4];
#elif METRIC == METRIC_LLCM_PER_SIZE
    return g_tier_manager.obj_vector[obj_index].metrics.loads_count[4]/(g_tier_manager.obj_vector[obj_index].size/GB);
#elif METRIC == METRIC_ABS_TLB_MISS
    double all_tlb_miss = 0;
    int i;
    for(i=0; i<MEM_LEVELS;i++){
        all_tlb_miss += g_tier_manager.obj_vector[obj_index].metrics.tlb_miss[i];
    }
    return all_tlb_miss;
#elif METRIC == METRIC_TLB_MISS_PER_SIZE
    double all_tlb_miss = 0;
    int i;
    for(i=0; i<MEM_LEVELS;i++){
        all_tlb_miss += g_tier_manager.obj_vector[obj_index].metrics.tlb_miss[i];
    }
    return all_tlb_miss/(g_tier_manager.obj_vector[i].size/GB);
#elif METRIC == METRIC_ABS_WRITE
    return g_tier_manager.obj_vector[obj_index].metrics.stores_count;
#elif METRIC == METRIC_WRITE_PER_SIZE
    return g_tier_manager.obj_vector[obj_index].metrics.stores_count/(g_tier_manager.obj_vector[obj_index].size/GB);
#elif METRIC == METRIC_ABS_LATENCY
    double all_latency = 0;
    int i;
    for(i=0; i<MEM_LEVELS;i++){
        all_latency += g_tier_manager.obj_vector[obj_index].metrics.sum_latency_cost[i];
    }
    return all_latency;
#elif METRIC == METRIC_LATENCY_PER_SIZE
    double all_latency = 0;
    int i;
    for(i=0; i<MEM_LEVELS;i++){
        all_latency += g_tier_manager.obj_vector[obj_index].metrics.sum_latency_cost[i];
    }
    return all_latency/(g_tier_manager.obj_vector[i].size/GB);

#endif
    
}
static void sort_objects(void){
    int i,j;
    int aux;
    
    for(i=0;i<MAX_OBJECTS;i++){
        g_key_value.key = i;
        g_key_value.value = get_hotness_metric(i);
        g_sorted_obj[i] = g_key_value;
    }
    
    qsort (g_sorted_obj, sizeof(g_sorted_obj)/sizeof(*g_sorted_obj), sizeof(*g_sorted_obj), comp);
}
int register_on_file(int interval, int index, unsigned long int addr, float size, double metric_value, float *status_memory_pages){
    int i;
    struct timespec timestamp;
    
    clock_gettime(CLOCK_REALTIME, &timestamp);
    fprintf(g_fp, "%d,%lu.%lu,%d,%p,%.2lf,%.2lf,",interval,timestamp.tv_sec, timestamp.tv_nsec, index, addr, size, metric_value);
    for(i=0; i<g_num_nodes_available; i++)
    {
         fprintf(g_fp, "%.2f,",status_memory_pages[i]);
    }
    fprintf(g_fp, "%.2f\n",status_memory_pages[i]);
    
    //clean
    for(i=0 ;i< g_num_nodes_available + 1; i++){
        status_memory_pages[i] = 0;
    }
}
void check_and_register_decisions(int app_pid, float *status_memory_pages){
    int i;
    int j;
    static int counter=0;
    double metric_value;
    
    counter++;
    
    for(j=0;j<MAX_OBJECTS;j++){
        //i = g_sorted_obj[j].key;
        metric_value = get_hotness_metric(j);
        //if(g_tier_manager.obj_alloc[i] == 1 && g_sorted_obj[j].value >= g_hotness_threshold){
        if(g_tier_manager.obj_alloc[j] == 1 && metric_value >= g_hotness_threshold){

            query_status_memory_pages(app_pid, \
                                      g_tier_manager.obj_vector[j].start_addr, \
                                      g_tier_manager.obj_vector[j].size, \
                                      status_memory_pages);
            register_on_file(counter,\
                             g_tier_manager.obj_vector[j].obj_index,\
                             g_tier_manager.obj_vector[j].start_addr, \
                             g_tier_manager.obj_vector[j].size/GB, \
                             /*g_sorted_obj[j].value, \*/
                             metric_value, \
                             status_memory_pages);
        }
    }
    
}
void *thread_track_mapping(void *_args){
    float *status_memory_pages = NULL;//The last position is to save unmapped pages
    //int pid = g_tier_manager.pids_to_manager[0];//I'm assuming only one single application to monitor
    int app_pid=-1;
    FILE *fptr;
    
    //calculate the total number of numa nodes
    set_number_of_nodes_availables();
    status_memory_pages = malloc((g_num_nodes_available + 1) * sizeof(float));
    
    if(status_memory_pages == NULL){
        fprintf(stderr, "Error during malloc to status_memory_pages\n");
        exit(-1);
    }
    if(open_output_file() != 0){
        fprintf(stderr, "Error : open_output_file()\n");
        exit(-1);
    }
    
    while(app_pid == -1){
        fptr = fopen("pid.txt", "r");
        if(fptr != NULL )
        {
            fscanf(fptr, "%d", &app_pid);
            fseek(fptr, 0, SEEK_SET);
        }
    }
    
    
    while(g_running){
        //sort_objects();
        check_and_register_decisions(app_pid, status_memory_pages);
        sleep(g_track_mapping_interval);
        
    }
}
