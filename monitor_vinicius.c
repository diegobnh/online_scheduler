#define _GNU_SOURCE 1
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>

#include <assert.h>
#include <linux/perf_event.h>
#include <perf/core.h>
#include <perf/cpumap.h>
#include <perf/event.h>
#include <perf/evlist.h>
#include <perf/evsel.h>
#include <perf/mmap.h>
#include <perf/threadmap.h>
#include <perfmon/pfmlib_perf_event.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <pthread.h>
#include "recorder.h"
#include "sample_processor.h"
#define STORAGE_ID "SHM_TEST"

struct schedule_manager *g_shared_memory; //variable that stores the contents of the shared memory
int g_fd_shared_memory ; //file descriptor to shared memory
static int g_running = 1;

void close_monitor(int signum, siginfo_t *info, void *uc){
    fprintf(stderr, "[monitor], [sample_processor] closed! \n");
    g_running = 0;
}
void clear_number_of_samples(int curr_ring_index){
	int i;
	int j;
	
	for(i=0; i< g_shared_memory->tier[0].num_obj; i++){
            g_shared_memory->tier[0].obj_vector[i].ring.stores_count[curr_ring_index] = 0;
            
            for(j=0 ; j< MEM_LEVELS; j++){
                g_shared_memory->tier[0].obj_vector[i].ring.sum_latency_cost[curr_ring_index][j] = 0;
                g_shared_memory->tier[0].obj_vector[i].ring.loads_count[curr_ring_index][j] = 0;
                g_shared_memory->tier[0].obj_vector[i].ring.TLB_hit[curr_ring_index][j] = 0;
                g_shared_memory->tier[0].obj_vector[i].ring.TLB_miss[curr_ring_index][j] = 0;
            }
    }
    for(i=0; i< g_shared_memory->tier[1].num_obj; i++){
            g_shared_memory->tier[1].obj_vector[i].ring.stores_count[curr_ring_index] = 0;
            
            for(j=0 ; j< MEM_LEVELS; j++){
                g_shared_memory->tier[1].obj_vector[i].ring.sum_latency_cost[curr_ring_index][j] = 0;
                g_shared_memory->tier[1].obj_vector[i].ring.loads_count[curr_ring_index][j] = 0;
                g_shared_memory->tier[1].obj_vector[i].ring.TLB_hit[curr_ring_index][j] = 0;
                g_shared_memory->tier[1].obj_vector[i].ring.TLB_miss[curr_ring_index][j] = 0;
            }
    }

}
int get_vector_index(long long chave, int *tier_type){
    int i;
    
    *tier_type = 0;
    for(i = 0; i < g_shared_memory->tier[0].num_obj; i++){
        if(chave >= g_shared_memory->tier[0].obj_vector[i].start_addr && chave <= g_shared_memory->tier[0].obj_vector[i].end_addr){
            if(g_shared_memory->tier[0].obj_flag_alloc[i] == 1){
                return i;
            }
        }
    }
    
    *tier_type = 1;
    for(i = 0; i < g_shared_memory->tier[1].num_obj; i++){
       if(chave >= g_shared_memory->tier[1].obj_vector[i].start_addr && chave <= g_shared_memory->tier[1].obj_vector[i].end_addr){
           if(g_shared_memory->tier[1].obj_flag_alloc[i] == 1){
               return i;
           }
       }
    }
    
    return -1;
}

void setup_shared_memory(void){
    g_fd_shared_memory = shm_open(STORAGE_ID, O_RDWR | O_CREAT | O_EXCL, 0660);
    g_fd_shared_memory = shm_open(STORAGE_ID, O_RDWR, 0);
    g_shared_memory = mmap(0,sizeof(struct schedule_manager),PROT_READ|PROT_WRITE,MAP_SHARED,g_fd_shared_memory,0);

}

char *concat(const char *s1, const char *s2)
{
    char *result = malloc(strlen(s1) + strlen(s2) + 1);
    if (result == NULL) {
        return "malloc failed in concat\n";
    }
    strcpy(result, s1);
    strcat(result, s2);
    return result;
}

int is_served_by_local_cache1(union perf_mem_data_src data_src)
{
    if (data_src.mem_lvl & PERF_MEM_LVL_HIT) {
        if (data_src.mem_lvl & PERF_MEM_LVL_L1) {
            return 1;
        }
    }
    return 0;
}

int is_served_by_local_cache2(union perf_mem_data_src data_src)
{
    if (data_src.mem_lvl & PERF_MEM_LVL_HIT) {
        if (data_src.mem_lvl & PERF_MEM_LVL_L2) {
            return 1;
        }
    }
    return 0;
}

int is_served_by_local_cache3(union perf_mem_data_src data_src)
{
    if (data_src.mem_lvl & PERF_MEM_LVL_HIT) {
        if (data_src.mem_lvl & PERF_MEM_LVL_L3) {
            return 1;
        }
    }
    return 0;
}

int is_served_by_local_lfb(union perf_mem_data_src data_src)
{
    if (data_src.mem_lvl & PERF_MEM_LVL_HIT) {
        if (data_src.mem_lvl & PERF_MEM_LVL_LFB) {
            return 1;
        }
    }
    return 0;
}

int is_served_by_local_cache(union perf_mem_data_src data_src)
{
    if (data_src.mem_lvl & PERF_MEM_LVL_HIT) {
        if (data_src.mem_lvl & PERF_MEM_LVL_L1) {
            return 1;
        }
        if (data_src.mem_lvl & PERF_MEM_LVL_LFB) {
            return 1;
        }
        if (data_src.mem_lvl & PERF_MEM_LVL_L2) {
            return 1;
        }
        if (data_src.mem_lvl & PERF_MEM_LVL_L3) {
            return 1;
        }
    }
    return 0;
}

int is_served_by_local_memory(union perf_mem_data_src data_src)
{
    if (data_src.mem_lvl & PERF_MEM_LVL_HIT) {
        if (data_src.mem_lvl & PERF_MEM_LVL_LOC_RAM) {
            return 1;
        }
    }
    return 0;
}

int is_served_by_remote_cache_or_local_memory(union perf_mem_data_src data_src)
{
    if (data_src.mem_lvl & PERF_MEM_LVL_HIT && data_src.mem_lvl & PERF_MEM_LVL_REM_CCE1) {
        return 1;
    }
    return 0;
}

int is_served_by_remote_memory(union perf_mem_data_src data_src)
{
    if (data_src.mem_lvl & PERF_MEM_LVL_HIT) {
        if (data_src.mem_lvl & PERF_MEM_LVL_REM_RAM1) {
            return 1;
        } else if (data_src.mem_lvl & PERF_MEM_LVL_REM_RAM2) {
            return 1;
        }
    }
    return 0;
}

int is_served_by_local_NA_miss(union perf_mem_data_src data_src)
{
    if (data_src.mem_lvl & PERF_MEM_LVL_NA) {
        return 1;
    }
    if (data_src.mem_lvl & PERF_MEM_LVL_MISS && data_src.mem_lvl & PERF_MEM_LVL_L3) {
        return 1;
    }
    return 0;
}

//char *get_data_src_opcode(union perf_mem_data_src data_src)
int get_data_src_opcode(union perf_mem_data_src data_src)
{
    //char *res = concat("", "");
    //char *old_res;
    
    //Isso não deveria se rum if/else ao invés de apenas ifs
    if (data_src.mem_op & PERF_MEM_OP_NA) {
        //old_res = res;
        //res = concat(res, "NA");
        //free(old_res);
        return 0;
    }
    if (data_src.mem_op & PERF_MEM_OP_LOAD) {
        //old_res = res;
        //res = concat(res, "Load");
        //free(old_res);
        return 1;
    }
    if (data_src.mem_op & PERF_MEM_OP_STORE) {
        //old_res = res;
        //res = concat(res, "Store");
        //free(old_res);
        return 2;
    }
    if (data_src.mem_op & PERF_MEM_OP_PFETCH) {
        //old_res = res;
        //res = concat(res, "Prefetch");
        //free(old_res);
        return 3;
    }
    if (data_src.mem_op & PERF_MEM_OP_EXEC) {
        //old_res = res;
        //res = concat(res, "Exec code");
        //free(old_res);
        return 4;
    }
    //return res;
}

//char *get_data_src_dtlb(union perf_mem_data_src data_src)
int get_data_src_dtlb(union perf_mem_data_src data_src)
{
	/*
    char *res = concat("", "");
    char *old_res;
    if (data_src.mem_dtlb & PERF_MEM_TLB_NA) {
        old_res = res;
        res = concat(res, "TLB_NA");
        free(old_res);
    } else if (data_src.mem_dtlb & PERF_MEM_TLB_L1) {
        old_res = res;
        res = concat(res, "TLB_L1");
        free(old_res);
    } else if (data_src.mem_dtlb & PERF_MEM_TLB_L2) {
        old_res = res;
        res = concat(res, "TLB_L2");
        free(old_res);
    } else if (data_src.mem_dtlb & PERF_MEM_TLB_WK) {
        old_res = res;
        res = concat(res, "TLB_WK");
        free(old_res);
    } else if (data_src.mem_dtlb & PERF_MEM_TLB_OS) {
        old_res = res;
        res = concat(res, "TLB_OS");
        free(old_res);
    } else {
        old_res = res;
        res = concat(res, "TLB_Unknown");
        //printf("dtlb: %d\n", data_src.dtlb);
        free(old_res);
    }
    */
    if (data_src.mem_dtlb & PERF_MEM_TLB_HIT) {
        //old_res = res;
        //res = concat(res, "_HIT");
        //free(old_res);
        return 1;
    } else if (data_src.mem_dtlb & PERF_MEM_TLB_MISS) {
        //old_res = res;
        //res = concat(res, "_MISS");
        //free(old_res);
        return 2;
    }
    return -1;
    //return res;
}

char *get_data_src_level(union perf_mem_data_src data_src)
{
    char *res = concat("", "");
    char *old_res;
    if (data_src.mem_lvl & PERF_MEM_LVL_NA) {
        old_res = res;
        res = concat(res, "NA");
        free(old_res);
    }
    if (data_src.mem_lvl & PERF_MEM_LVL_L1) {
        old_res = res;
        res = concat(res, "L1");
        free(old_res);
    } else if (data_src.mem_lvl & PERF_MEM_LVL_LFB) {
        old_res = res;
        res = concat(res, "LFB");
        free(old_res);
    } else if (data_src.mem_lvl & PERF_MEM_LVL_L2) {
        old_res = res;
        res = concat(res, "L2");
        free(old_res);
    } else if (data_src.mem_lvl & PERF_MEM_LVL_L3) {
        old_res = res;
        res = concat(res, "L3");
        free(old_res);
    } else if (data_src.mem_lvl & PERF_MEM_LVL_LOC_RAM) {
        old_res = res;
        res = concat(res, "Local_RAM");
        free(old_res);
    } else if (data_src.mem_lvl & PERF_MEM_LVL_REM_RAM1) {
        old_res = res;
        res = concat(res, "Remote_RAM_1_hop");
        free(old_res);
    } else if (data_src.mem_lvl & PERF_MEM_LVL_REM_RAM2) {
        old_res = res;
        res = concat(res, "Remote_RAM_2_hops");
        free(old_res);
    } else if (data_src.mem_lvl & PERF_MEM_LVL_REM_CCE1) {
        old_res = res;
        res = concat(res, "Remote_Cache_1_hop");
        free(old_res);
    } else if (data_src.mem_lvl & PERF_MEM_LVL_REM_CCE2) {
        old_res = res;
        res = concat(res, "Remote_Cache_2_hops");
        free(old_res);
    } else if (data_src.mem_lvl & PERF_MEM_LVL_IO) {
        old_res = res;
        res = concat(res, "I/O_Memory");
        free(old_res);
    } else if (data_src.mem_lvl & PERF_MEM_LVL_UNC) {
        old_res = res;
        res = concat(res, "Uncached_Memory");
        free(old_res);
    } else {
        old_res = res;
        res = concat(res, "Unknown");
        //printf("mem_lvl: %d\n", data_src.mem_lvl);
        free(old_res);
    }
    if (data_src.mem_lvl & PERF_MEM_LVL_HIT) {
        old_res = res;
        res = concat(res, "_Hit");
        free(old_res);
    } else if (data_src.mem_lvl & PERF_MEM_LVL_MISS) {
        old_res = res;
        res = concat(res, "_Miss");
        free(old_res);
    }
    return res;
}

static int libperf_print(enum libperf_print_level level, const char *fmt, va_list ap)
{
    return vfprintf(stderr, fmt, ap);
}

union u64_swap {
    __u64 val64;
    __u32 val32[2];
};

int encode_event(const char *event_name)
{
    pfm_perf_encode_arg_t perf_encoded;
    struct perf_event_attr perf_attr;
    memset(&perf_encoded, 0, sizeof(perf_encoded));
    perf_encoded.size = sizeof(perf_encoded);
    perf_encoded.attr = &perf_attr;
    memset(&perf_attr, 0, sizeof(perf_attr));

    int ret = pfm_get_os_event_encoding(event_name, PFM_PLM3, PFM_OS_PERF_EVENT,
                                        &perf_encoded);
    if (ret != PFM_SUCCESS) {
        fprintf(stderr, "cannot encode event %s: %s", event_name, pfm_strerror(ret));
        return -1;
    }

    return perf_attr.config;
}

int main(int argc, char **argv)
{
    struct perf_evlist *evlist;
    struct perf_evsel *load_evsel;
    struct perf_evsel *store_evsel;
    struct perf_mmap *map;
    struct perf_cpu_map *cpus;
    
    int incremental_ring_index=0;
    int curr_ring_index;
    int vector_index;
    int mem_level;//L1=0, LFB=1, L2=2, L3=3, DRAM=4
    int mem_type_oper; //load or store
    int tlb_type;
    int tier_type;//0 to dram , 1 to pmem
    int i, j, w;
    
    struct sigaction sa;
    sa.sa_sigaction = close_monitor;
    sa.sa_flags = SA_SIGINFO;
    
    if (sigaction(SIGPROF, &sa, NULL) < 0) { //SIGPROF Profiling timer expired
         fprintf(stderr,"Error setting up signal handler\n");
         exit(1);
    }
    
    setup_shared_memory();
    
    //pthread_create(&g_sample_processor, NULL, thread_sample_processor, g_shared_memory);

    int curr_err = pfm_initialize();
    if (curr_err != PFM_SUCCESS) {
        return -1;
    }

    int load_event = encode_event("MEM_TRANS_RETIRED:LOAD_LATENCY:ldlat=3");
    assert(load_event != -1);

    int store_event = encode_event("MEM_UOPS_RETIRED:ALL_STORES");
    assert(store_event != -1);

    struct perf_event_attr load_attr = {
        .type = PERF_TYPE_RAW,
        //.config = 0x1cd,      // see libpfm4
        .config = load_event,
        .disabled = 1,
        .precise_ip = 2,
        .exclude_kernel = 1,
        .exclude_user = 0,
        .exclude_hv = 1,
        .freq = 1,
        .sample_freq = 100,
        //.sample_period = 1000,
        //.sample_type =
        .sample_type = PERF_SAMPLE_IDENTIFIER | PERF_SAMPLE_ADDR | PERF_SAMPLE_WEIGHT | PERF_SAMPLE_DATA_SRC,
    };
    struct perf_event_attr store_attr = {
        .type = PERF_TYPE_RAW,
        //.config = 0x82d0,      // see libpfm4
        .config = store_event,
        .disabled = 1,
        .precise_ip = 2,
        .exclude_kernel = 1,
        .exclude_user = 0,
        .exclude_hv = 1,
        .freq = 1,
        .sample_freq = 100,
        //.sample_period = 1000,
        .sample_type = PERF_SAMPLE_IDENTIFIER | PERF_SAMPLE_ADDR | PERF_SAMPLE_WEIGHT | PERF_SAMPLE_DATA_SRC,
    };

    int err = -1;
    union perf_event *event;

    libperf_init(libperf_print);

    cpus = perf_cpu_map__new(NULL);
    if (!cpus) {
        fprintf(stderr, "failed to create cpus\n");
        return -1;
    }

    evlist = perf_evlist__new();
    if (!evlist) {
        fprintf(stderr, "failed to create evlist\n");
        goto out_cpus;
    }

    load_evsel = perf_evsel__new(&load_attr);
    if (!load_evsel) {
        fprintf(stderr, "failed to create load event\n");
        goto out_cpus;
    }
    store_evsel = perf_evsel__new(&store_attr);
    if (!store_evsel) {
        fprintf(stderr, "failed to create load event\n");
        goto out_cpus;
    }

    perf_evlist__add(evlist, load_evsel);
    perf_evlist__add(evlist, store_evsel);

    perf_evlist__set_maps(evlist, cpus, NULL);

    err = perf_evlist__open(evlist);
    if (err) {
        fprintf(stderr, "failed to open evlist\n");
        goto out_evlist;
    }

    err = perf_evlist__mmap(evlist, 64);
    if (err) {
        fprintf(stderr, "failed to mmap evlist\n");
        goto out_evlist;
    }

    while(g_running) {
        perf_evlist__enable(evlist);
        sleep(5);
        perf_evlist__disable(evlist);
        
        curr_ring_index = incremental_ring_index % RING_BUFFER_SIZE;
        
        pthread_mutex_lock(&g_shared_memory->global_mutex);

        clear_number_of_samples(curr_ring_index);

        perf_evlist__for_each_mmap(evlist, map, false) {
            if (perf_mmap__read_init(map) < 0)
                continue;

            while ((event = perf_mmap__read_event(map)) != NULL) {
                const __u32 type = event->header.type;
                //int cpu, pid, tid;
                __u64 sample_addr, sample_id, weight, *array;
                union u64_swap u;
                union perf_mem_data_src data_src;

                if (type != PERF_RECORD_SAMPLE) {
                    perf_mmap__consume(map);
                    continue;
                }

                array = event->sample.array;

                sample_id = *array;
                array++;

                sample_addr = *array;
                array++;
                
                vector_index = get_vector_index(sample_addr, &tier_type);

                weight = *array;
                array++;

                data_src.val = *array;
                
                //vector_index = -1 means sample didn't match with any one object from application
                if (vector_index != -1){
                    mem_type_oper = get_data_src_opcode(data_src);
                    mem_level = -1;
                    
                    //1 is load 
                    if(mem_type_oper == 1){
                    	if (is_served_by_local_NA_miss(data_src)) {
                	    	mem_level = -1;
                		}
                		if (is_served_by_local_cache1(data_src)) {
                    		mem_level = 0;
                		}
                		if (is_served_by_local_lfb(data_src)) {
                    		mem_level = 1;
              		  	}
                		if (is_served_by_local_cache2(data_src)) {
                    		mem_level = 2;
                		}
                		if (is_served_by_local_cache3(data_src)) {
                    		mem_level = 3;
                		}
                		if (is_served_by_local_memory(data_src)) {
                    		mem_level = 4;
                		}
                		
                		if(mem_level != -1){
                			tlb_type = get_data_src_dtlb(data_src);
                			if(tlb_type == 1){
                				g_shared_memory->tier[tier_type].obj_vector[vector_index].ring.TLB_hit[curr_ring_index][mem_level]++;
                			}else if(tlb_type == 2){
                				g_shared_memory->tier[tier_type].obj_vector[vector_index].ring.TLB_miss[curr_ring_index][mem_level]++;
                            }else{
                                fprintf(stderr, "get_data_src_dtlb() is returning -1\n");
                            }
                		}
                		
                    }else if(mem_type_oper == 2){ // 2 is store
                    	g_shared_memory->tier[tier_type].obj_vector[vector_index].ring.stores_count[curr_ring_index]++;
                    
                    }
                }

                perf_mmap__consume(map);
            }
            perf_mmap__read_done(map);
        }
        
        
        for(i=0; i< g_shared_memory->tier[0].num_obj; i++){
            fprintf(stderr, "DRAM Object i=%d\n",i);
            for(w=4; w< MEM_LEVELS; w++){
                fprintf(stderr, "\tw=%d\n",w);
                for(j=0; j< RING_BUFFER_SIZE; j++){
                    fprintf(stderr, "\t\tj=%d, ",j);
                    fprintf(stderr, "lat:%ld, load:%ld, tlb_miss:%ld, tlb_hit:%ld\n", \
                                     g_shared_memory->tier[0].obj_vector[i].ring.sum_latency_cost[j][w],\
                                     g_shared_memory->tier[0].obj_vector[i].ring..loads_count[j][w],\
                                     g_shared_memory->tier[0].obj_vector[i].ring..TLB_hit[j][w],\
                                     g_shared_memory->tier[0].obj_vector[i].ring..TLB_miss[j][w]);
                }
            }
        }
        pthread_mutex_unlock(&g_shared_memory->global_mutex);
        
        incremental_ring_index++;
    }

  out_evlist:
    perf_evlist__delete(evlist);
  out_cpus:
    perf_cpu_map__put(cpus);
    return err;
}
