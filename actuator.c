

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

#define NODE_0_DRAM 0
#define NODE_0_PMEM 2
#define NODE_1_DRAM 1
#define NODE_1_PMEM 3

#define ROUND_ROBIN 1
#define RANDOM 2
#define FIRST_DRAM 3
#define FIRST_PMEM 4


#define METRIC_ABS_LLCM 1
#define METRIC_LLCM_PER_SIZE 2
#define METRIC_ABS_TLB_MISS 3
#define METRIC_TLB_MISS_PER_SIZE 4
#define METRIC_ABS_WRITE 5
#define METRIC_WRITE_PER_SIZE 6
#define METRIC_ABS_LATENCY 7
#define METRIC_LATENCY_PER_SIZE 8


#define NUMA_NODES_AVAILABLE 3

#ifndef INIT_DATAPLACEMENT
#error "INIT_DATAPLACEMENT is not defined."
#endif

#if !(INIT_DATAPLACEMENT == 1 || INIT_DATAPLACEMENT == 2 || INIT_DATAPLACEMENT == 3 || INIT_DATAPLACEMENT == 4)
#error "INIT_DATAPLACEMENT value invalid."
#endif

#ifndef METRIC
#error "METRIC is not defined."
#endif

#if !(METRIC >= 1 && METRIC <= 8)
#error "METRIC value invalid."
#endif

#define rmb()   asm volatile("lfence" ::: "memory")

#define DEBUG
#ifdef DEBUG
  #define D if(1)
#else
  #define D if(0)
#endif




int g_iteration=0;
double g_current_dram_consumption = 0;
double g_current_pmem_consumption = 0;
double g_current_free_dram_space = 0;
static int g_hotness_threshold = 1;

extern float g_actuator_interval;
extern float g_dram_capacity;
extern float g_dram_capacity;
extern float g_minimum_space_to_active_downgrade;
extern tier_manager_t g_tier_manager;
extern volatile sig_atomic_t g_running;
int g_pipe_write_fd;
int g_pipe_read_fd;



struct key_value{
    int index;
    double value;
};

struct key_value g_key_value;
struct key_value g_sorted_obj[MAX_OBJECTS];


int guard(int ret, char *err){
    if (ret == -1)
    {
        perror(err);
        return -1;
    }
    return ret;
}
void calculate_DRAM_consumption(void){
    int i;
    
    g_current_dram_consumption = 0;
    for(i=0;i<MAX_OBJECTS;i++){
        if(g_tier_manager.obj_alloc[i] == 1 &&  g_tier_manager.obj_status[i] == NODE_0_DRAM){
            g_current_dram_consumption += g_tier_manager.obj_vector[i].size;
        }else{
            g_current_pmem_consumption += g_tier_manager.obj_vector[i].size/GB;
        }
    }
    
    g_current_dram_consumption = g_current_dram_consumption/GB;
    g_current_free_dram_space = g_dram_capacity - g_current_dram_consumption ;
    if(g_current_free_dram_space < 0){
        g_current_free_dram_space = 0;
    }
    D fprintf(stderr, "---------------------------------------------------------------[DRAM_consumption] Free:%.2lf Consumed:%.2lf\n", g_current_free_dram_space, g_current_dram_consumption);
}
int comp(const void * elem1, const void * elem2){
    struct key_value *k_v1, *k_v2;
    
    k_v1 = (struct key_value*)elem1;
    k_v2 = (struct key_value*)elem2;
    
    double f = k_v1->value;
    double s = k_v2->value;
    
    if (f < s) return  1;
    if (f > s) return -1;
    return 0;
}
double get_hotness_metric(int obj_index){
    
#if METRIC == METRIC_ABS_LLCM
    return g_tier_manager.obj_vector[obj_index].metrics.loads_count[4];
#elif METRIC == METRIC_LLCM_PER_SIZE
    return g_tier_manager.obj_vector[obj_index].metrics.loads_count[4]/(g_tier_manager.obj_vector[obj_index].size/GB);
#elif METRIC == METRIC_ABS_TLB_MISS
    double all_tlb_miss = 0;
    int i;
    for(i=0; i<MEM_LEVELS;i++){
        all_tlb_miss += g_tier_manager.obj_vector[obj_index].metrics.TLB_miss[i];
    }
    return all_tlb_miss;
#elif METRIC == METRIC_TLB_MISS_PER_SIZE
    double all_tlb_miss = 0;
    int i;
    for(i=0; i<MEM_LEVELS;i++){
        all_tlb_miss += g_tier_manager.obj_vector[obj_index].metrics.TLB_miss[i];
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
void sort_objects(void){
    int i,j;
    int aux;
    
    for(i=0;i<MAX_OBJECTS;i++){
        g_key_value.index = i;
        //g_key_value.value = g_tier_manager.obj_vector[i].metrics.loads_count[4]/(g_tier_manager.obj_vector[i].size/GB);
        g_key_value.value = get_hotness_metric(i);
        g_sorted_obj[i] = g_key_value;
    }
    
    qsort (g_sorted_obj, sizeof(g_sorted_obj)/sizeof(*g_sorted_obj), sizeof(*g_sorted_obj), comp);
}
void my_send_bind(unsigned long start_addr, unsigned long size ,int target_node, int obj_index){
    data_bind_t data;
    
    data.obj_index = obj_index;
    data.start_addr = start_addr;
    data.size = size;
    data.nodemask_target_node = 1<<target_node;

    write(g_pipe_write_fd, &data, sizeof(data_bind_t));
}
int initial_dataplacement_policy(unsigned long start_addr, unsigned long size, int obj_index){
    static int memory_index = 0;
    int flag_dram_alloc = 0;
    
#if INIT_DATAPLACEMENT == ROUND_ROBIN
    if(((memory_index ++) %2)){
#elif INIT_DATAPLACEMENT == RANDOM
    if(rand() % 2){
#elif INIT_DATAPLACEMENT == FIRST_DRAM
    if(1){
#elif INIT_DATAPLACEMENT == FIRST_PMEM
    if(0){
#endif
        if((size/GB) + g_current_dram_consumption < g_dram_capacity){
            my_send_bind(start_addr, size, NODE_0_DRAM, obj_index);
            return NODE_0_DRAM;
        }else{
            flag_dram_alloc = 0;
        }
    }
    if(flag_dram_alloc == 0)
    {
        my_send_bind(start_addr, size, NODE_0_PMEM, obj_index);
        return NODE_0_PMEM;
    }
    
}
void check_initial_dataplacement_and_desalocations(void){
    int i;
    int node_bind;

    rmb();
    
    for(i=0;i<MAX_OBJECTS;i++){
        if(g_tier_manager.obj_alloc[i] == 1 &&  g_tier_manager.obj_status[i] == -1){ //require first bind
            node_bind = initial_dataplacement_policy(g_tier_manager.obj_vector[i].start_addr, g_tier_manager.obj_vector[i].size, i);
            g_tier_manager.obj_status[i] = node_bind;
            
            //my_send_bind(g_tier_manager.obj_vector[i].start_addr, g_tier_manager.obj_vector[i].size, NODE_0_PMEM, i); //This instruction is problematic!!
            //g_tier_manager.obj_status[i] = NODE_0_PMEM;
            
        }else if(g_tier_manager.obj_alloc[i] == 0  && g_tier_manager.obj_status[i] != -1){
            g_tier_manager.obj_status[i] = -1;
        }
    }
}
int check_candidates_to_migration(void){
    int i;
    int j;
    int flag_has_value_in_metric = 0;
    
    check_initial_dataplacement_and_desalocations();

    //fprintf(stderr, "Iteration %d\n", g_iteration);
    for(j=0;j<MAX_OBJECTS;j++){
        i = g_sorted_obj[j].index;
        if(g_tier_manager.obj_alloc[i] == 1 && g_tier_manager.obj_status[i] == NODE_0_DRAM && g_sorted_obj[j].value >= g_hotness_threshold){
                D fprintf(stderr, "DRAM[%d,%p,%.4lf] = %.2lf\n", \
                        g_tier_manager.obj_vector[i].obj_index, \
                        g_tier_manager.obj_vector[i].start_addr,\
                        g_tier_manager.obj_vector[i].size/GB,\
                        g_sorted_obj[j].value);
        }
            
    }
        
    D fprintf(stderr, "\n");
    
    for(j=0;j<MAX_OBJECTS;j++){
        i = g_sorted_obj[j].index;
        if(g_tier_manager.obj_alloc[i] == 1 && g_tier_manager.obj_status[i] == NODE_0_PMEM && g_sorted_obj[j].value >= g_hotness_threshold){
            D fprintf(stderr, "PMEM[%d,%p, %06.4lf] = %.2lf\n", \
                    g_tier_manager.obj_vector[i].obj_index, \
                    g_tier_manager.obj_vector[i].start_addr,\
                    g_tier_manager.obj_vector[i].size/GB,\
                    g_sorted_obj[j].value);
            
            flag_has_value_in_metric = 1;
        }
    }
    
    return flag_has_value_in_metric;
}
//Chamada quando tem espaço na DRAM e objeto hot no pmem
void policy_migration_promotion(void){
    int i;
    int j;
    unsigned long nodemask;
    int num_obj_migrated=0;
    float metric_value;
    double free_dram_space = g_current_free_dram_space;
    struct timespec timestamp;

    D fprintf(stderr, "[Promotion] Obj. Promoted: ", g_current_free_dram_space, g_current_dram_consumption);
    //Percorre todos os objetos que possuem um LLCM mínimo e que ainda estão alocados
    //Depois verifica se ele também cabe no espaço atual de memória
    data_bind_t data;
    
    for(j=0;j<MAX_OBJECTS;j++){
        i = g_sorted_obj[j].index;
        
        metric_value = g_sorted_obj[j].value;
        if(g_tier_manager.obj_alloc[i] == 1 && g_tier_manager.obj_status[i] == NODE_0_PMEM && metric_value > g_hotness_threshold){
        //if(g_tier_manager.obj_vector[i].alloc_flag  == 1 && g_tier_manager.obj_vector[i].status == NODE_0_PMEM && llcm > g_hotness_threshold){
            if ((g_tier_manager.obj_vector[i].size/GB) < free_dram_space){
               
                my_send_bind(g_tier_manager.obj_vector[i].start_addr, g_tier_manager.obj_vector[i].size, NODE_0_DRAM, i);
                g_tier_manager.obj_status[i] = NODE_0_DRAM;
                
                free_dram_space -= g_tier_manager.obj_vector[i].size/GB;
                D fprintf(stderr, "%d " , i);
                num_obj_migrated++;
            }
        }
    }
    clock_gettime(CLOCK_REALTIME, &timestamp);
    D fprintf(stderr, " -> Total promoted:%d , %lu.%lu\n", num_obj_migrated, timestamp.tv_sec, timestamp.tv_nsec);
    fflush(stderr);
    
}

int calculate_total_active_pages(unsigned long int addr, unsigned long int size, int node_to_count){
    int status_memory_pages[4]={0,0,0,0};
    
    if ((numa_available() < 0)) {
        fprintf(stderr, "error: not a numa machine\n");
        return -1;
    }
        
    unsigned node = 0;

    int num_nodes = numa_max_node() + 1;
    if (num_nodes < 2) {
        fprintf(stderr, "error: a minimum of 2 nodes is required\n");
        exit(1);
    }
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
    
    if (numa_move_pages(getpid(), page_count, pages_addr, NULL, status, MPOL_MF_MOVE) == -1) {
        fprintf(stderr, "error code: %d\n", errno);
        perror("error description:");
    }
    
    for (int i = 0; i < page_count; i++) {
        if(status[i] >= 0 && status[i] < NUMA_NODES_AVAILABLE){
            status_memory_pages[status[i]]++;//0,1,2
        }else{
            status_memory_pages[3]++;
        }
    }
    return status_memory_pages[node_to_count];
}
        
        
int decide_demotion_migration(int *list_obj_index, int pmem_candidate_index, float gain_metric_factor){
    int i=0;
    int index;
    int sum_pages_dram = 0;
    int sum_pages_pmem = 0;
    float sum_stores_dram = 0;
    float sum_stores_pmem = 0;
    
    
    while(list_obj_index[i] != -1){
        index = list_obj_index[i];
        sum_pages_dram += calculate_total_active_pages(g_tier_manager.obj_vector[index].start_addr, g_tier_manager.obj_vector[index].size,NODE_0_DRAM);
        sum_stores_dram += g_tier_manager.obj_vector[index].metrics.stores_count;
        i++;
    }
    
    sum_pages_pmem = calculate_total_active_pages(g_tier_manager.obj_vector[pmem_candidate_index].start_addr, g_tier_manager.obj_vector[pmem_candidate_index].size,NODE_0_PMEM);
    sum_stores_pmem = g_tier_manager.obj_vector[pmem_candidate_index].metrics.stores_count;
    
    //As páginas as ativas estão sempre retornando zero ????
    //fprintf(stderr, "Gain factor:%.2lf\n", gain_metric_factor);
    //fprintf(stderr, "Total Active Pages on DRAM:%d , Stores:%.2f\n", sum_pages_dram, sum_stores_dram);
    //fprintf(stderr, "Total Active Pages on PMEM:%d , Stores:%.2f\n", sum_pages_pmem, sum_stores_pmem);
    return 1;
}
//Chamada quando não tem espaço na DRAM
int policy_migration_demotion(void){
    int i;
    int j;
    float g_current_free_dram_space;
    unsigned long nodemask;
    int pmem_candidate_index;
    float pmem_candidate_metric;
    float pmem_candidate_size;
    int num_obj_migrated=0;
    float curr_metric;
    float sum_metric_candidates_demotion = 0;
    int list_obj_index[MAX_OBJECTS];
    int cont;
    int curr_index;
    float gain_metric_factor;
    
    //First get the PMEM object candidate
    pmem_candidate_index = -1;
    
    for(j=0;j<MAX_OBJECTS;j++){
        i = g_sorted_obj[j].index;
        
        if(g_sorted_obj[i].value >= g_hotness_threshold && g_tier_manager.obj_alloc[i] == 1 && g_tier_manager.obj_status[i] == NODE_0_PMEM){
            pmem_candidate_size = g_sorted_obj[j].value;
            if(pmem_candidate_size < g_dram_capacity){
                pmem_candidate_metric = g_sorted_obj[j].value;
                pmem_candidate_index = i;
                
                D fprintf(stderr, "\n[Demotion] PMEM candidate index to promotion:%d, %.2lf, size:%.2lf\n", i, pmem_candidate_metric, pmem_candidate_size);
                break;
            }
        }
    }
    
    if(pmem_candidate_index == -1)
        return 0;
    
     
    //Second, check how many object are necessary to demotion from DRAM
    list_obj_index[0] = -1;
    cont = 0;
    //Stay in the loop until achieve space necessary to move PMEM object candidate
    for(j=MAX_OBJECTS-1; j>= 0; j--){
        i = g_sorted_obj[j].index;
        
        curr_metric = g_sorted_obj[j].value;
        if(g_tier_manager.obj_alloc[i] == 1 && g_tier_manager.obj_status[i] == NODE_0_DRAM){
            sum_metric_candidates_demotion += curr_metric;
            pmem_candidate_size -= g_tier_manager.obj_vector[i].size/GB;
            list_obj_index[cont] = i;
            //fprintf(stderr, "list_obj_index[%d] = %d \t addr: %p, size:%.4lf\n", cont, g_tier_manager.obj_vector[i].index_id,\
                        g_tier_manager.obj_vector[i].start_addr, g_tier_manager.obj_vector[i].size/GB);
            cont++;
            if(pmem_candidate_size <= 0){
                break;
            }
        }
    }
    list_obj_index[cont] = -1;//To know where i should stop
    
    //for(i=0; list_obj_index[i] !=-1; i++){
    //    D fprintf(stderr, "\tObj to remove from DRAM : %d\n", list_obj_index[i]);
    //}
    //D fprintf(stderr,"\tSomatorio dos LLCM :%.2lf, PMEM candidate:%.2lf\n", sum_metric_candidates_demotion, pmem_candidate_metric);
     
    //Third, decide if the swap will ocurr
    if(sum_metric_candidates_demotion != 0){
        gain_metric_factor = (float)pmem_candidate_metric/sum_metric_candidates_demotion;
    }else{
        gain_metric_factor = (float)pmem_candidate_metric;
    }
    
    if(sum_metric_candidates_demotion < pmem_candidate_metric){
        if(decide_demotion_migration(list_obj_index, pmem_candidate_index, gain_metric_factor)){
            cont = 0;
            while(list_obj_index[cont] != -1){
                curr_index = list_obj_index[cont];
                
                my_send_bind(g_tier_manager.obj_vector[curr_index].start_addr, g_tier_manager.obj_vector[curr_index].size, NODE_0_PMEM, curr_index);
                g_tier_manager.obj_status[curr_index] = NODE_0_PMEM;
         
                cont++;
                num_obj_migrated++;
            }
        }
    }
    //else{
    //    D fprintf(stderr,"Sum of all objs from DRAM has more value (%.2lf) than Top1 from PMEM (%.2lf)!!\n",sum_metric_candidates_demotion, pmem_candidate_metric);
    //}
    
    D fprintf(stderr, "Num obj demoted:%d\n", num_obj_migrated);
    
}
void check_migration_error(void){
    /*
    1 << 0 = `0000 0001`
    1 << 1 = `0000 0010`
    1 << 2 = `0000 0100`
     
    We are goint to read the node in bitmask format. So, we need how to convert
    bitmask to node.
    1 - means node 0 - DRAM local
    2 - means node 1 - DRAM remote - we are not working with this option
    4 - means node 2 - PMEM local
     
    In preload, the write will send this message:
        - write(g_pipe_write_fd, &data, sizeof(data_bind_t));
    */
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
            //D fprintf(stderr, "[actuator] Detected bind error: %p %lu ", buf.start_addr, buf.size);
            
            for(i=0;i<MAX_OBJECTS;i++){
                if(g_tier_manager.obj_vector[i].start_addr == buf.start_addr && g_tier_manager.obj_vector[i].size == buf.size){
                    if(g_tier_manager.obj_alloc[i] == 1){
                        //D fprintf(stderr, " - record undone to object:%d\n", i);
                        if(buf.nodemask_target_node == 1){
                            g_tier_manager.obj_status[i] = NODE_0_PMEM;
                        }else if(buf.nodemask_target_node == 4){
                            g_tier_manager.obj_status[i] = NODE_0_DRAM;
                        }
                    }
                }
            }
            num_read = read(g_pipe_read_fd, &buf, sizeof(data_bind_t));
        }
    }
}

void open_pipes(void){

    char FIFO_PATH_MIGRATION[50];
    char FIFO_PATH_MIGRATION_ERROR[50];
    
    sprintf(FIFO_PATH_MIGRATION, "/tmp/migration.%d", g_tier_manager.pids_to_manager[0]);
    //guard(mkfifo(FIFO_PATH_MIGRATION, 0777), "Could not create pipe");
    g_pipe_write_fd = guard(open(FIFO_PATH_MIGRATION, O_WRONLY), "[actuator] Could not open pipe MIGRATION for writing");

    sleep(1); //time to preload create the named pipe;
    
    sprintf(FIFO_PATH_MIGRATION_ERROR, "/tmp/migration_error.%d", g_tier_manager.pids_to_manager[0]);
    g_pipe_read_fd = guard(open(FIFO_PATH_MIGRATION_ERROR, O_RDONLY | O_NONBLOCK), "[actuator] Could not open pipe MIGRATION_ERROR for reading");
}
void *thread_actuator(void *_args){
    int i,j;
    int flag_has_value_in_metric;
    struct timespec start, end;
    
    open_pipes();
    while(g_running){
        check_migration_error();
        calculate_DRAM_consumption();
        sort_objects();
        flag_has_value_in_metric = check_candidates_to_migration();
        
        check_migration_error();
        calculate_DRAM_consumption();
        
        if(flag_has_value_in_metric == 1 && g_current_free_dram_space > 0)  {
            policy_migration_promotion();//move top objects from PMEM to DRAM
        }
        calculate_DRAM_consumption();
                
        if(g_current_free_dram_space <= g_minimum_space_to_active_downgrade){
            policy_migration_demotion();//move non-top objetcts from DRAM to PMEM
        }
        //D fprintf(stderr, "-----------------------------------------------------------------------------\n");
        
        sleep(g_actuator_interval);
        
    }//while end
    
    //Print how was the final allocation including objects with 0 hotness
    //g_hotness_threshold = 0;
    //check_candidates_to_migration();
    
}




