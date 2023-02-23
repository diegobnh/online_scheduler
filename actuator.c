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
#include "actuator.h"
#include "monitor.h"
#include "recorder.h"

#ifndef METRIC
#error "METRIC is not defined."
#endif

#if !(METRIC >= 1 && METRIC <= 8)
#error "METRIC value invalid."
#endif

#define rmb()   asm volatile("lfence" ::: "memory")

extern float g_hotness_threshold;
extern float g_actuator_interval;
extern tier_manager_t g_tier_manager;
extern volatile sig_atomic_t g_running_actuator;
extern double g_start_free_DRAM;
extern int g_app_pid;

int g_iteration = 0;
int g_pipe_write_fd;
int g_pipe_read_fd;
float g_median_metric;
int g_promotion_flag;
int g_has_demotion;
int g_active_obj;
double g_curr_max_llcm = 1;
double g_curr_max_tlbm = 1;
double g_curr_max_stores = 1;

float *g_status_memory_pages = NULL;//The last vector position is to save unmapped pages
int g_num_nodes_available;

struct key_value{
    int key; //key will be the the index object
    double value;
    double llcm;//this is just for trace
    double stores;//this is just for trace
    double tlbm;//this is just for trace
};
static struct key_value g_key_value_list[MAX_OBJECTS];


double static get_my_timestamp(void){
    char buf[256];
    char cmd[256];
    double timestamp;
    FILE *stream_file;

    sprintf(cmd, "awk '{print $1}' /proc/uptime");

    if (NULL == (stream_file = popen(cmd, "r"))) { perror("popen"); exit(EXIT_FAILURE);
    }

    fgets(buf, sizeof(buf), stream_file);
    sscanf(buf, "%lf", &timestamp);

    return timestamp;
}
float static get_free_dram_space(){
    //return current free dram space in Gigabytes
    
    char buf[256];
    char cmd[256];
    double current_dram_consumption;
    double current_free_dram_space;
    FILE *stream_file;

    sprintf(cmd, "numastat -p %d -c | grep Private | awk '{print $2}' ", g_app_pid);

    if (NULL == (stream_file = popen(cmd, "r"))) {
        perror("popen");
        exit(EXIT_FAILURE);
    }

    fgets(buf, sizeof(buf), stream_file);
    sscanf(buf, "%lf",&current_dram_consumption);//measured in megabytes

    current_dram_consumption = (current_dram_consumption/1000.0);//convert to GB
    current_free_dram_space = g_start_free_DRAM - current_dram_consumption;

    fclose(stream_file);
#ifdef DEBUG
    fprintf(stderr, "[actuator] %lf free dram space(gb):%.2lf\n", get_my_timestamp(), current_free_dram_space);
#endif
    return current_free_dram_space;
}
int static guard(int ret, char *err){
    if (ret == -1)
    {
        perror(err);
        return -1;
    }
    return ret;
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
static void sort_objects(void){
    int i;
    int j;
    int obj_index;
    int count = 0;
    double all_tlb_miss;
    double llcm;
    double tlbm;
    double stores;
    double timestamp;
    struct key_value aux;
    
    for(i=0; i<g_tier_manager.total_obj;i++){
        if(g_tier_manager.obj_alloc[obj_index] == 1){
            aux.key = i;
            
            llcm = g_tier_manager.obj_vector[i].metrics.loads_count[4]; // /(g_tier_manager.obj_vector[i].size/GB);
            //llcm = g_tier_manager.obj_vector[i].metrics.loads_count[4]/(g_tier_manager.obj_vector[i].size/GB);
            
            all_tlb_miss = 0;
            for(j=0; j<MEM_LEVELS;j++){
                all_tlb_miss += g_tier_manager.obj_vector[i].metrics.tlb_miss[j];
            }
            
            //tlbm = all_tlb_miss;
            tlbm = g_tier_manager.obj_vector[i].metrics.tlb_miss[4];
            
            stores = g_tier_manager.obj_vector[i].metrics.stores_count;
            //stores = g_tier_manager.obj_vector[i].metrics.stores_count/(g_tier_manager.obj_vector[i].size/GB);
            
            //Store is the unique that we accumulate over time. All the other we update using average moving
            //aux.value = llcm + (tlb_miss * 2) ; //+ (stores * 2) ;
            //aux.value = llcm;  //+ tlb_miss;
            if(llcm > g_curr_max_llcm){
                g_curr_max_llcm = llcm;
            }
            if(tlbm > g_curr_max_tlbm){
                g_curr_max_tlbm = tlbm;
            }
            if(stores > g_curr_max_stores){
                g_curr_max_stores = stores;
            }
            aux.value = (llcm/g_curr_max_llcm) + (tlbm/g_curr_max_tlbm) + (stores/g_curr_max_stores); //metric maximum value is 2
            
            //only for tracing
            aux.llcm = llcm;
            aux.stores = stores;
            aux.tlbm = tlbm;
            
            g_key_value_list[count] = aux;
            
            count ++;
        }
    }
    g_active_obj = count;
    
    qsort (g_key_value_list, sizeof(g_key_value_list)/sizeof(*g_key_value_list), sizeof(*g_key_value_list), comp);
    
    g_promotion_flag = 0;
    timestamp = get_my_timestamp();
    for(i=0; i<g_active_obj;i++){
        obj_index = g_key_value_list[i].key;
        if(g_key_value_list[i].value > g_hotness_threshold){
            g_promotion_flag = 1;
#ifdef DEBUG
            fprintf(stderr, "[actuator] hotness %lf, %d, %p, DRAM=%.2f, NVM=%.2f, SIZE=%.2lf, LLCM=%.2lf, TLBM=%.2lf, STORE=%.2lf METRIC_NORMALIZED=%.2lf\n", \
                      timestamp, \
                      obj_index, \
                      g_tier_manager.obj_vector[obj_index].start_addr, \
                      g_tier_manager.obj_vector[obj_index].page_status[0], \
                      g_tier_manager.obj_vector[obj_index].page_status[2], \
                      g_tier_manager.obj_vector[obj_index].size/GB, \
                      g_key_value_list[i].llcm, \
                      g_key_value_list[i].tlbm, \
                      g_key_value_list[i].stores, \
                      g_key_value_list[i].value);
#endif
        }
    }
}
void static bind_order(unsigned long start_addr, unsigned long size ,int target_node, int obj_index, int type){
    data_bind_t data;
    
    data.obj_index = obj_index;
    data.start_addr = start_addr;
    data.size = size;
    data.nodemask_target_node = 1<<target_node;
    data.type = type;
    data.flag = 1 << 1; //#define MPOL_MF_MOVE     (1<<1)    /* Move pages owned by this process to conform*/

    write(g_pipe_write_fd, &data, sizeof(data_bind_t));
}
void policy_promotion(void){
    int i;
    int obj_index;
    float max_migration_gb = CHUNK_SIZE/GB;
    unsigned long nodemask;
    double current_free_dram = get_free_dram_space();
    float metric_value;
    float curr_alloc_size_gb;
    data_bind_t data;

    //Iterates over all objects from hottest to least hot.
    for(i = 0; i < g_active_obj;i++){
        obj_index = g_key_value_list[i].key;
        curr_alloc_size_gb = (g_tier_manager.obj_vector[obj_index].size/GB) * (g_tier_manager.obj_vector[obj_index].page_status[2]);

        if(curr_alloc_size_gb > 0 && curr_alloc_size_gb < current_free_dram && g_tier_manager.obj_vector[obj_index].last_decision != NODE_0_DRAM){
            if(g_key_value_list[i].value > g_hotness_threshold && g_tier_manager.obj_alloc[obj_index] == 1){ //&& g_key_value_list[i].tlbm > g_hotness_threshold){
                 //if (curr_alloc_size_gb <= max_migration_gb){  //!= g_current_obj_undone_mapping){
#ifdef DEBUG
                    fprintf(stderr, "[actuator] promotion, %lf, %d, %p, SIZE=%.2lf DRAM=%.2lf, NVM=%.2lf, SIZE=%.2lf, LLCM:%.2lf, TLBM:%.2lf, METRIC_NORMALIZED:%.2lf LAST_DECISION:%d\n", \
                              get_my_timestamp(), \
                              obj_index, \
                              g_tier_manager.obj_vector[obj_index].start_addr, \
                              curr_alloc_size_gb, 
                              g_tier_manager.obj_vector[obj_index].page_status[0], \
                              g_tier_manager.obj_vector[obj_index].page_status[2], \
                              curr_alloc_size_gb, \
                              g_key_value_list[i].llcm,\
                              g_key_value_list[i].tlbm, \
                              g_key_value_list[i].value, \
                              g_tier_manager.obj_vector[obj_index].last_decision);
#endif
                    bind_order(g_tier_manager.obj_vector[obj_index].start_addr, g_tier_manager.obj_vector[obj_index].size, NODE_0_DRAM, obj_index, PROMOTION);
                    g_tier_manager.obj_vector[obj_index].last_decision = NODE_0_DRAM;
                    g_tier_manager.obj_vector[obj_index].birth_date = get_my_timestamp();
                
                    max_migration_gb -= curr_alloc_size_gb;
                    if(max_migration_gb < 0){
                        break;
                    }
            }
            /*
            else{
                fprintf(stderr, "[actuator] NOTpromotion %d, %p, SIZE=%.2lf, DRAM=%.2lf, NVM=%.2lf, LLCM=%.2lf METRIC_NORMALIZED=%.2lf last_decision=%d\n", \
                          obj_index, \
                          g_tier_manager.obj_vector[obj_index].start_addr, \
                          curr_alloc_size_gb, \
                          g_tier_manager.obj_vector[obj_index].page_status[0], \
                          g_tier_manager.obj_vector[obj_index].page_status[2], \
                          g_key_value_list[i].llcm, \
                          g_key_value_list[i].value, \
                          g_tier_manager.obj_vector[obj_index].last_decision);
                
            }
            */
        }
    }
}
void policy_demotion(void){
    /*
     Remove all object whose metric (llcm + tlbm) is less than 100 and has size greater than 100MB.
     There is a maximum transfer limit of one object after reaching a chunk of migrated data.
     */
    int i;
    int obj_index;
    int min_lifetime_sec = 5;
    float max_migration_gb = CHUNK_SIZE/GB;
    float curr_alloc_size_gb;
    float metric_value;
    float min_size_gb = 0.1;
    double passed_time;
    
    g_has_demotion = 0;
    //iterate over active objects
    for(i = g_active_obj - 1; i >= 0; i--){
        obj_index = g_key_value_list[i].key;
        curr_alloc_size_gb = (g_tier_manager.obj_vector[obj_index].size/GB) * (g_tier_manager.obj_vector[obj_index].page_status[0]);
        
        if(curr_alloc_size_gb > min_size_gb){
            passed_time = get_my_timestamp() - g_tier_manager.obj_vector[obj_index].birth_date;
            if(passed_time > min_lifetime_sec && g_key_value_list[i].value < g_hotness_threshold && g_tier_manager.obj_vector[obj_index].last_decision != NODE_0_PMEM){
                if(g_tier_manager.obj_alloc[obj_index] == 1){
#ifdef DEBUG
                    fprintf(stderr, "[actuator] demotion %lf, %d, %p, SIZE=%.2lf, LIFETIME=%.2lf, DRAM=%.2lf, NVM=%.2lf, LLCM=%.2lf, TLBM:%.2lf, METRIC_NORMALIZED:%.2lf\n", \
                              get_my_timestamp(), \
                              obj_index, \
                              g_tier_manager.obj_vector[obj_index].start_addr, \
                              curr_alloc_size_gb, \
                              passed_time, \
                              g_tier_manager.obj_vector[obj_index].page_status[0], \
                              g_tier_manager.obj_vector[obj_index].page_status[2], \
                              g_key_value_list[i].llcm, \
                              g_key_value_list[i].tlbm, \
                              g_key_value_list[i].value);
#endif
                    g_has_demotion = 1;
                    bind_order(g_tier_manager.obj_vector[obj_index].start_addr, g_tier_manager.obj_vector[obj_index].size, NODE_0_PMEM, obj_index, DEMOTION);
                    g_tier_manager.obj_vector[obj_index].last_decision = NODE_0_PMEM;
                    max_migration_gb -= curr_alloc_size_gb;
                    if(max_migration_gb < 0){
                        break;
                    }

                }
            }
            /*
            else{
                fprintf(stderr, "[actuator] NOTdemotion %d, %p, SIZE=%.2lf, LIFETIME=%.2lf, DRAM=%.2lf, NVM=%.2lf, LLCM=%.2lf METRIC_NORMALIZED=%.2lf last_decision=%d\n", \
                          obj_index, \
                          g_tier_manager.obj_vector[obj_index].start_addr, \
                          curr_alloc_size_gb, \
                          passed_time, \
                          g_tier_manager.obj_vector[obj_index].page_status[0], \
                          g_tier_manager.obj_vector[obj_index].page_status[2], \
                          g_key_value_list[i].llcm, \
                          g_key_value_list[i].value, \
                          g_tier_manager.obj_vector[obj_index].last_decision);
            }
            */
        }
    }
}

void static actuator_open_pipes(){
    char FIFO_PATH_MIGRATION[50];
    char FIFO_PATH_MIGRATION_ERROR[50];
    
    sprintf(FIFO_PATH_MIGRATION, "migration.pipe");
    g_pipe_write_fd = guard(open(FIFO_PATH_MIGRATION, O_WRONLY), "[actuator] Could not open pipe MIGRATION for writing");

    //If you want non-blocking mode, you need to make sure the reader opens the fifo before the writer
    sprintf(FIFO_PATH_MIGRATION_ERROR, "migration_error.pipe", g_app_pid);
    g_pipe_read_fd = guard(open(FIFO_PATH_MIGRATION_ERROR, O_RDONLY | O_NONBLOCK), "[actuator] Could not open pipe MIGRATION_ERROR for reading");
}
static int query_status_memory_pages(int pid, unsigned long int addr, unsigned long int size){
    unsigned node = 0;
    int i;
    
    // size must be page-aligned
    size_t pagesize = getpagesize();
    if((size % pagesize) != 0){
        for(int i = 0; i < g_num_nodes_available + 1; i++){
            g_status_memory_pages[i] = -1;
        }
        return -1;
    }
    //assert((size % pagesize) == 0);

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
        fprintf(stderr, "[preload - numa_move_pages] error code: %d\n", errno);
        perror("error description:");
    }

    for (int i = 0; i < page_count; i++) {
        if(status[i] >= 0 && status[i] < g_num_nodes_available){
            g_status_memory_pages[status[i]]++;//0,1,2
        }else{
            g_status_memory_pages[g_num_nodes_available]++;
        }
    }

    for(int i = 0; i < g_num_nodes_available + 1; i++){
        g_status_memory_pages[i] = (float)g_status_memory_pages[i]/page_count;
    }
    return page_count;
}
int _set_number_of_nodes_availables(void){
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
void allocate_vector_status(void){
    _set_number_of_nodes_availables();
    
    g_status_memory_pages = malloc((g_num_nodes_available + 1) * sizeof(float));

    if(g_status_memory_pages == NULL){
        fprintf(stderr, "Error during malloc to status_memory_pages\n");
        exit(-1);
    }
}
void read_allocation_status(void){
    int i;
    int j;
    struct timespec g_start, g_end;
    int total_obj = 0;
    unsigned long page_count = 0;
    
    clock_gettime(CLOCK_REALTIME, &g_start);
    for(i = 0; i < g_tier_manager.total_obj;i++){
        if(g_tier_manager.obj_alloc[i] == 1){
            page_count += query_status_memory_pages(g_app_pid, g_tier_manager.obj_vector[i].start_addr, g_tier_manager.obj_vector[i].size);
            total_obj++;
            for(j=0; j<= g_num_nodes_available; j++){
                g_tier_manager.obj_vector[i].page_status[j] = g_status_memory_pages[j];
            }
        }
    }
    clock_gettime(CLOCK_REALTIME, &g_end);

    //uint64_t delta_us = (g_end.tv_sec - g_start.tv_sec) * 1000000 + (g_end.tv_nsec - g_start.tv_nsec) / 1000;
    //fprintf(stderr, "kernel_cost, %d, %lu, %.2lf\n", total_obj, page_count, delta_us/1000.0);
}
void check_migration_error(void){
    int i;
    data_bind_t buf;
    ssize_t num_read = 0;
    
    num_read = read(g_pipe_read_fd, &buf, sizeof(data_bind_t));

    if(num_read== -1 && errno == EAGAIN){
        //D fprintf(stderr, "There is no data to read!\n");
    }else{
        //Preciso procurar o objeto com essas características e desfazer o bind registrado!
        //Ou seja, se estiver 1, significa que continuou no PMEM
        //Se tiver 4 signfifica que continuou na DRAM
        while(num_read > 0){
            if(g_tier_manager.obj_alloc[buf.obj_index] == 1 && buf.type != INITIAL_DATAPLACEMENT){
                g_tier_manager.obj_vector[buf.obj_index].last_decision = -1;
#ifdef DEBUG
                fprintf(stderr, "undone object:%d\n", buf.obj_index);
#endif
            }
            num_read = read(g_pipe_read_fd, &buf, sizeof(data_bind_t));
        }
    }
}

void *thread_actuator(void *_args){
    int count=2;
    
    actuator_open_pipes();
    allocate_vector_status();
    
    while(g_app_pid == -1){
        sleep(0.1);
    }
    
    while(get_free_dram_space() > ((2 * CHUNK_SIZE)/GB) && g_running_actuator != 0){
        sleep(0.01);
    }
    fprintf(stderr, "[actuator] %lf Starting to try demotion objects\n", get_my_timestamp());
    g_has_demotion = 0;
    
    while(g_running_actuator){
        
        sort_objects();
        
        if(get_free_dram_space() < (2 * CHUNK_SIZE)/GB){
            if(g_has_demotion == 0){
                read_allocation_status();
                policy_demotion();
                count = 0; //não tinha essa linha
            }else{
                count++;
                if(count >= 2 ){//wait 3 periods after demotion
                    g_has_demotion=0;
                }
            }
        }
        if(g_promotion_flag == 1){
            read_allocation_status();
            policy_promotion();
        }
        //check_migration_error();
        sleep(g_actuator_interval);
    }
}




