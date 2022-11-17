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

#ifndef INIT_DATAPLACEMENT
#error "INIT_DATAPLACEMENT is not defined."
#endif

#if !(INIT_DATAPLACEMENT >= 1 && INIT_DATAPLACEMENT <= 4)
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


struct key_value{
    int key; //key will be the the index object
    double value;
    double llcm;//this is just for trace
    double stores;//this is just for trace
    double tlbm;//this is just for trace
};

extern int g_hotness_threshold;
extern float g_actuator_interval;
extern tier_manager_t g_tier_manager;
extern volatile sig_atomic_t g_running;

float g_dram_pressure_threshold = 0.90;
int g_app_pid = -1;
int g_iteration = 0;
double g_current_dram_consumption;
double g_start_free_DRAM;
double g_current_free_dram_space;
int g_pipe_write_fd;
int g_pipe_read_fd;
float g_median_metric;

static struct key_value g_key_value_list[MAX_OBJECTS];

int guard(int ret, char *err){
    if (ret == -1)
    {
        perror(err);
        return -1;
    }
    return ret;
}
void set_current_free_dram(void){
    char buf[256];
    char cmd[256];
    
    FILE *stream_file;
    
    sprintf(cmd, "grep MemFree /sys/devices/system/node/node0/meminfo | awk '{print $4/1000}' ");

    if (NULL == (stream_file = popen(cmd, "r"))) {
        perror("popen");
        exit(EXIT_FAILURE);
    }

    fgets(buf, sizeof(buf), stream_file);
    sscanf(buf, "%lf",&g_start_free_DRAM);//measured in megabytes
    
    g_start_free_DRAM = (g_start_free_DRAM/1000.0); //convert to GB
    g_current_free_dram_space = g_start_free_DRAM;
    
    fclose(stream_file);
    D fprintf(stderr, "Free DRAM start:%.4lf(GB)\n",g_start_free_DRAM);
}
void update_free_dram_space(void){
    char buf[256];
    char cmd[256];
    static int count = 0;
    
    count++;
    
    FILE *stream_file;
    
    sprintf(cmd, "numastat -p %d -c | grep Private | awk '{print $2}' ", g_app_pid);

    if (NULL == (stream_file = popen(cmd, "r"))) {
        perror("popen");
        exit(EXIT_FAILURE);
    }

    fgets(buf, sizeof(buf), stream_file);
    sscanf(buf, "%lf",&g_current_dram_consumption);//measured in megabytes
    
    g_current_dram_consumption = (g_current_dram_consumption/1000.0);//convert to GB
    g_current_free_dram_space = g_start_free_DRAM - g_current_dram_consumption;
    
    fclose(stream_file);
    if(count%10 == 0)
        D fprintf(stderr, "App DRAM usage:%.4lf (GB)\n",g_current_dram_consumption);
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
    return g_tier_manager.obj_vector[obj_index].metrics.loads_count[4]; //4 is the index of last level cache
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
    int i, j;
    double all_tlb_miss;

    double llcm_per_size;
	double tlb_miss_per_size;
    double stores_per_size;
    struct key_value aux;
    
    for(i=0;i<MAX_OBJECTS;i++){
        aux.key = i;
        
        llcm_per_size = g_tier_manager.obj_vector[i].metrics.loads_count[4]/(g_tier_manager.obj_vector[i].size/GB);
        
        all_tlb_miss = 0;
        for(j=0; j<MEM_LEVELS;j++){
            all_tlb_miss += g_tier_manager.obj_vector[i].metrics.tlb_miss[j];
        }
        //tlb_miss_per_size = g_tier_manager.obj_vector[i].metrics.tlb_miss[4]/(g_tier_manager.obj_vector[i].size/GB);//4 means DRAM
        tlb_miss_per_size = all_tlb_miss/(g_tier_manager.obj_vector[i].size/GB);
        stores_per_size = g_tier_manager.obj_vector[i].metrics.stores_count/(g_tier_manager.obj_vector[i].size/GB);
        
        //Store is the unique that we accumulate over time. All the other we update using average moving
        aux.value = llcm_per_size + (tlb_miss_per_size * 2) + (stores_per_size * 3) ;
        
        //only for tracing
        aux.llcm = llcm_per_size;
        aux.stores = stores;
        aux.tlbm = tlb_miss;
        
        g_key_value_list[i] = aux;
        if(g_key_value_list[i].value > 0){
            fprintf(stderr, "[sort_objects] Index:%d, Value:%.2lf\n", i, g_key_value_list[i].value);
        }
    }
    
    qsort (g_key_value_list, sizeof(g_key_value_list)/sizeof(*g_key_value_list), sizeof(*g_key_value_list), comp);
}

void bind_order(unsigned long start_addr, unsigned long size ,int target_node, int obj_index){
    data_bind_t data;
    
    data.obj_index = obj_index;
    data.start_addr = start_addr;
    data.size = size;
    data.nodemask_target_node = 1<<target_node;

    write(g_pipe_write_fd, &data, sizeof(data_bind_t));
}
int initial_dataplacement_policy(unsigned long start_addr, unsigned long size, int sliced, int obj_index){
    static int memory_index = 0;
    int flag_dram_alloc = 0;
    struct timespec timestamp;
    
#if INIT_DATAPLACEMENT == ROUND_ROBIN
    if(((memory_index ++) %2)){
#elif INIT_DATAPLACEMENT == RANDOM
    if(rand() % 2){
#elif INIT_DATAPLACEMENT == FIRST_DRAM
    if(1){
#elif INIT_DATAPLACEMENT == BASED_ON_SIZE
    if(size < CHUNK_SIZE){
#endif
        if((g_current_free_dram_space - (size/GB)) > 0 && sliced == 0){
            clock_gettime(CLOCK_REALTIME, &timestamp);
            D fprintf(stderr, "[actuator] initial_data_placement_dram, %d, %p, %.4lf (GB)\n", obj_index, start_addr, (size/GB));
            bind_order(start_addr, size, NODE_0_DRAM, obj_index);
            return NODE_0_DRAM;
        }
    }
    clock_gettime(CLOCK_REALTIME, &timestamp);
    D fprintf(stderr, "[actuator] initial_data_placement_pmem, %d, %p, %.4lf (GB)\n", obj_index, start_addr, (size/GB));
    bind_order(start_addr, size, NODE_0_PMEM, obj_index);
    return NODE_0_PMEM;
}
void check_initial_dataplacement_and_desalocations(void){
    int i;
    int node_bind;
    
    rmb();

    for(i=0;i<MAX_OBJECTS;i++){
        if(g_tier_manager.obj_alloc[i] == 1 &&  g_tier_manager.obj_status[i] == -1){ //require first bind
            node_bind = initial_dataplacement_policy(g_tier_manager.obj_vector[i].start_addr, g_tier_manager.obj_vector[i].size, g_tier_manager.obj_vector[i].sliced, i);
            g_tier_manager.obj_status[i] = node_bind;
            
        }else if(g_tier_manager.obj_alloc[i] == 0  && g_tier_manager.obj_status[i] != -1){
            g_tier_manager.obj_status[i] = -1;
        }
    }
    update_free_dram_space();
}
int check_candidates_to_promotion(void){
    int i;
    int j;
    int count=0;
    int flag_promotion = 0;
    float size_factor;
    float metric_factor;
    float factor;
    
    //Esse loop não é necessário depois. Hoje é apenas utilizado para fins de análise.
    //Remover por questão de performance
    for(j=0;j<MAX_OBJECTS;j++){
        i = g_key_value_list[j].key;
        if(g_tier_manager.obj_alloc[i] == 1 && g_tier_manager.obj_status[i] == NODE_0_DRAM && g_key_value_list[j].value > g_hotness_threshold){
            //D fprintf(stderr, "[actuator] Hottest Object in DRAM, %d, %p,%.2lf,%.2lf\n", \
                        g_tier_manager.obj_vector[i].obj_index, \
                        g_tier_manager.obj_vector[i].start_addr,\
                        g_tier_manager.obj_vector[i].size/GB,\
                        g_key_value_list[j].value);
            count++;
            //if (count > 10) break;
        }
    }
    if(count > 0){
        g_median_metric = g_key_value_list[count/2].value;
        D fprintf(stderr, "Mediana Objects in DRAM, llcm:%.2lf, store:%.2lf, tlbm:%.2lf\n", \
                  g_key_value_list[count/2].llcm, \
                  g_key_value_list[count/2].stores, \
                  g_key_value_list[count/2].tlbm,\
                  g_key_value_list[count/2].value);
    }
    //Esse for pode ser otimizado depois usando o break. Por enquanto fica melhor do jeito que está para visualizarmos os objetos candidatos.
    for(j=0;j<MAX_OBJECTS;j++){
        i = g_key_value_list[j].key;
        if(g_tier_manager.obj_alloc[i] == 1 && g_tier_manager.obj_status[i] == NODE_0_PMEM && g_key_value_list[j].value >= g_hotness_threshold){
            D fprintf(stderr, "\t[actuator - candidates from PMEM] %d, size:%.2lf, llcm:%.2lf, store:%.2lf, tlbm:%.2lf, value:%.2lf\n", \
                    g_tier_manager.obj_vector[i].obj_index, \
                    g_tier_manager.obj_vector[i].size/GB,\
                    g_key_value_list[j].llcm,\
                    g_key_value_list[j].stores,\
                    g_key_value_list[j].tlbm, \
                    g_key_value_list[j].value);
            
            size_factor = g_tier_manager.obj_vector[i].size/GB;
            metric_factor = g_median_metric/g_key_value_list[j].value;//if median in DRAM is less , so we have value under 1 which means will reduce the size factor
            factor = size_factor * metric_factor;
            
            D fprintf(stderr, "Factor:%.2lf  (Quanto maior o fator pior. Threshold:10)\n", factor);
            if(factor < 10)
                flag_promotion = 1;
            //break we can break if exist at least one
        }
    }
    
    return flag_promotion;
}
int check_candidates_to_demotion(void){
    int i;
    int j;
    int flag_demotion = 0;
    
    for(j=MAX_OBJECTS-1;j>=0;j--){
        i = g_key_value_list[j].key;
        if(g_tier_manager.obj_alloc[i] == 1 && g_tier_manager.obj_status[i] == NODE_0_DRAM && g_key_value_list[j].value < g_hotness_threshold){
            flag_demotion = 1;
            break;
        }
    }
    return flag_demotion;
}
//Chamada quando tem espaço na DRAM e objeto hot no pmem
void policy_promotion(void){
    int i;
    int obj_index;
    int max_migration_gb = CHUNK_SIZE;
    unsigned long nodemask;
    int num_obj_promoted=0;
    float metric_value;
    float curr_alloc_size_gb;
    double free_dram_space = g_current_free_dram_space;
    struct timespec timestamp;
    data_bind_t data;
    
    //Iterates over all objects from hottest to least hot.
    for(i=0;j<MAX_OBJECTS;i++){
        obj_index = g_key_value_list[i].key;
        metric_value = g_key_value_list[i].value;
	curr_alloc_size_gb = g_tier_manager.obj_vector[obj_index].size/GB
		
	//The maximum migration per instant of time is one CHUNK_SIZE	
        if(g_tier_manager.obj_alloc[obj_index] == 1 && g_tier_manager.obj_status[obj_index] == NODE_0_PMEM &&  curr_alloc_size_gb <= max_migration_gb){
        #if(g_tier_manager.obj_alloc[obj_index] == 1 && g_tier_manager.obj_status[obj_index] == NODE_0_PMEM && metric_value > g_hotness_threshold){
            if (curr_alloc_size_gb < free_dram_space){
                clock_gettime(CLOCK_REALTIME, &timestamp);
                D fprintf(stderr, "\t[actuator - promotion] %lu.%lu, Promotion obj: %d, metric:%lf, stores:%lf, tlb_miss:%lf\n",timestamp.tv_sec, timestamp.tv_nsec,i, metric_value, g_key_value_list[j].stores, g_key_value_list[j].tlbm);
                bind_order(g_tier_manager.obj_vector[obj_index].start_addr, g_tier_manager.obj_vector[obj_index].size, NODE_0_DRAM, obj_index);
                g_tier_manager.obj_status[obj_index] = NODE_0_DRAM;
                
                free_dram_space -= curr_alloc_size_gb;
		max_migration_gb -= curr_alloc_size_gb;
                num_obj_promoted++;
		    
		if(max_migration_gb == 0)
		    break;	
            }
        }
    }
    
    D fprintf(stderr, "\t[actuator - promotion] Total Promoted:%d\n", num_obj_promoted);
    update_free_dram_space();
    
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
        fprintf(stderr, "[actuator - numa_move_pages] error code: %d\n", errno);
        perror("error description:");
    }
    
    for (int i = 0; i < page_count; i++) {
        if(status[i] >= 0 && status[i] < num_nodes){
            status_memory_pages[status[i]]++;//0,1,2
        }else{
            status_memory_pages[3]++;
        }
    }
    return status_memory_pages[node_to_count];
}
//Chamada mesmo quando existe espaço na DRAM. A ideia é remover cold objetos da DRAM e evitar memory pressure.
void policy_demotion_cold_objects(void){
    int i;
    int j;
    int count_obj_demoted = 0;
    double current_free_dram = g_current_free_dram_space;
    struct timespec timestamp;
    
    
    for(j=MAX_OBJECTS-1;j>=0;j--){
        i = g_key_value_list[j].key;
        if(g_tier_manager.obj_alloc[i] == 1 && g_tier_manager.obj_status[i] == NODE_0_DRAM){
            if(g_key_value_list[j].value < g_hotness_threshold){
                clock_gettime(CLOCK_REALTIME, &timestamp);
                D fprintf(stderr, "\t[actuator - demotion 1] %lu.%lu, Demotion obj: %d\n",timestamp.tv_sec, timestamp.tv_nsec,i);
                bind_order(g_tier_manager.obj_vector[i].start_addr, g_tier_manager.obj_vector[i].size, NODE_0_PMEM, i);
                g_tier_manager.obj_status[i] = NODE_0_PMEM;
                count_obj_demoted++;
            }else{
                break;
            }
        }
    }
    D fprintf(stderr, "\t[actuator - demotion 1] Total Demoted:%d\n", count_obj_demoted);

    update_free_dram_space();
}
int decide_demotion_based_on_cost_benefit(int *list_obj_index, int pmem_candidate_index, float gain_metric_factor){
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
int policy_demotion_memory_pressure(void){
    int i;
    int j;
    unsigned long nodemask;
    int pmem_candidate_index;
    float pmem_candidate_metric;
    float pmem_candidate_size;
    int num_obj_demoted=0;
    float curr_metric;
    float sum_metric_candidates_demotion = 0;
    int list_obj_index[MAX_OBJECTS];
    int cont;
    int curr_index;
    float gain_metric_factor;
    struct timespec timestamp;
    
    //First get the PMEM object candidate
    pmem_candidate_index = -1;
    
    for(j=0;j<MAX_OBJECTS;j++){
        i = g_key_value_list[j].key;
        
        if(g_key_value_list[j].value >= g_hotness_threshold && g_tier_manager.obj_alloc[i] == 1 && g_tier_manager.obj_status[i] == NODE_0_PMEM){
            //pmem_candidate_size = g_key_value_list[j].value;
            pmem_candidate_size = g_tier_manager.obj_vector[i].size/GB;
            if(pmem_candidate_size < (g_start_free_DRAM - g_current_dram_consumption)){
                pmem_candidate_metric = g_key_value_list[j].value;
                pmem_candidate_index = i;
                
                D fprintf(stderr, "\t[actuator - demotion 2] Demotion PMEM candidate index to promotion:%d, %.2lf, size:%.2lf\n", i, pmem_candidate_metric, pmem_candidate_size);
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
        i = g_key_value_list[j].key;
        
        curr_metric = g_key_value_list[j].value;
        if(g_tier_manager.obj_alloc[i] == 1 && g_tier_manager.obj_status[i] == NODE_0_DRAM){
            sum_metric_candidates_demotion += curr_metric;
            pmem_candidate_size -= g_tier_manager.obj_vector[i].size/GB;
            list_obj_index[cont] = i;
            //fprintf(stderr, "list_obj_index[%d] = %d \t addr: %p, size:%.4lf\n", cont, g_tier_manager.obj_vector[i].key_id,\
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
        if(decide_demotion_based_on_cost_benefit(list_obj_index, pmem_candidate_index, gain_metric_factor)){
            cont = 0;
            while(list_obj_index[cont] != -1){
                curr_index = list_obj_index[cont];
                
                clock_gettime(CLOCK_REALTIME, &timestamp);
                D fprintf(stderr, "\t[actuator - demotion 2] %lu.%lu, Demotion obj: %d, %p\n",timestamp.tv_sec, timestamp.tv_nsec, curr_index, g_tier_manager.obj_vector[curr_index].start_addr);
                
                bind_order(g_tier_manager.obj_vector[curr_index].start_addr, g_tier_manager.obj_vector[curr_index].size, NODE_0_PMEM, curr_index);
                g_tier_manager.obj_status[curr_index] = NODE_0_PMEM;
         
                cont++;
                num_obj_demoted++;
            }
        }
    }
    //else{
    //    D fprintf(stderr,"Sum of all objs from DRAM has more value (%.2lf) than Top1 from PMEM (%.2lf)!!\n",sum_metric_candidates_demotion, pmem_candidate_metric);
    //}
    
    D fprintf(stderr, "\t[actuator - demotion 2] Total Demoted:%d\n", num_obj_demoted);
    update_free_dram_space();
    
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
                            D fprintf(stderr, "record undone to PMEM, obj_index:%d\n", i);
                        }else if(buf.nodemask_target_node == 4){
                            g_tier_manager.obj_status[i] = NODE_0_DRAM;
                            D fprintf(stderr, "record undone to DRAM, obj_index:%d\n", i);
                        }
                    }
                }
            }
            num_read = read(g_pipe_read_fd, &buf, sizeof(data_bind_t));
        }
    }
    //update_free_dram_space();
}

void open_pipes(){

    char FIFO_PATH_MIGRATION[50];
    char FIFO_PATH_MIGRATION_ERROR[50];
    
    //sprintf(FIFO_PATH_MIGRATION, "/tmp/migration.%d", g_tier_manager.pids_to_manager[0]);
    sprintf(FIFO_PATH_MIGRATION, "migration_%d.pipe", g_app_pid);
    //guard(mkfifo(FIFO_PATH_MIGRATION, 0777), "Could not create pipe");
    g_pipe_write_fd = guard(open(FIFO_PATH_MIGRATION, O_WRONLY), "[actuator] Could not open pipe MIGRATION for writing");

    sleep(1); //time to preload create the named pipe;
    
    //sprintf(FIFO_PATH_MIGRATION_ERROR, "/tmp/migration_error.%d", g_tier_manager.pids_to_manager[0]);
    sprintf(FIFO_PATH_MIGRATION_ERROR, "migration_error_%d.pipe", g_app_pid);
    g_pipe_read_fd = guard(open(FIFO_PATH_MIGRATION_ERROR, O_RDONLY | O_NONBLOCK), "[actuator] Could not open pipe MIGRATION_ERROR for reading");
}        
void *thread_actuator(void *_args){
    int i,j;
    int flag_promotion;
    int flag_demotion;
    struct timespec start;
    struct timespec end;
    FILE *fptr;
    
   
    while(g_app_pid == -1){
        fptr = fopen("pid.txt", "r");
        if(fptr != NULL )
        {
            fscanf(fptr, "%d", &g_app_pid);
            fseek(fptr, 0, SEEK_SET);
        }
    }
    
    sleep(1);
    open_pipes();
    set_current_free_dram();//setup the current DRAM available 
        
    while(g_running){
        check_initial_dataplacement_and_desalocations();
        /*
        check_migration_error();
        
        sort_objects();
        flag_promotion = check_candidates_to_promotion();
        
        if(flag_promotion == 1 && g_start_free_DRAM > 0)  {
            policy_promotion();//move top objects from PMEM to DRAM
            check_migration_error();
            D fprintf(stderr, "\n");
        }
        
        //Essa e a primeira forma de demotion. Remover antigos objetos hots. Ou seja, remover objetos colds.
        //Poderia pensar em algo do tipo , se ele for marcado por 5 iterações consecutivas sem atividade
        sort_objects();
        flag_demotion = check_candidates_to_demotion();
        if(flag_demotion == 1){
            policy_demotion_cold_objects();
            D fprintf(stderr, "\n");
        }
        
        //Esse segundo modo é quando a memória está próxima do limite. Nesse caso, faz-se troca de objetos mais hots por menos hots.
        if((g_current_dram_consumption/g_start_free_DRAM) > g_dram_pressure_threshold){
            policy_demotion_memory_pressure();//move non-top objetcts from DRAM to PMEM
            check_migration_error();
            D fprintf(stderr, "\n");
        }
        */
        sleep(g_actuator_interval);
        
    }//while end
}




