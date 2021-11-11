/*

sudo yum install libpfm*
gcc -o prog perf_event_open_sampling_mode.c -lpfm

This version is responsible to monitore loads and store only in CPU 0

*/


#define _GNU_SOURCE 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <unistd.h>
#include <fcntl.h>

#include <errno.h>

#include <signal.h>
#include <sys/mman.h>

#include <sys/ioctl.h>
#include <asm/unistd.h>
#include <sys/prctl.h>

#include <linux/perf_event.h>
#include <linux/version.h>
#include <time.h>
#include <inttypes.h>

#include <pthread.h>

#include <perfmon/pfmlib.h>
#include <perfmon/pfmlib_perf_event.h>

#define STORAGE_ID "SHM_TEST"
#include "monitor.h"
#include "time.h"
//#include "monitor_binary_search.h"
#include "recorder.h"
#include "actuator.h"
#include "sample_processor.h"


//#define DEBUG
#ifdef DEBUG
  #define D if(1)
#else
  #define D if(0)
#endif

#define SAMPLE_FREQUENCY 1000
#define NUMBER_SAMPLE_OVERFLOW 1000
#define MMAP_DATA_SIZE 1024

#define mb()    asm volatile("mfence":::"memory")
#define rmb()   asm volatile("lfence":::"memory")
#define wmb()   asm volatile("sfence" ::: "memory")



/*

Global variables start with sintaxe "g_name_of_variable"

*/
static int g_running = 1;
static int g_mmap_pages=1+MMAP_DATA_SIZE;
static int g_quiet = 1;
static int g_fd[2];

static int g_debug=0;
int g_sample_type = PERF_SAMPLE_TIME | PERF_SAMPLE_TID | PERF_SAMPLE_ADDR | PERF_SAMPLE_WEIGHT | PERF_SAMPLE_DATA_SRC ;

//specific variable to store ring buffer
volatile sig_atomic_t g_stores_count_overflow_events=0;
static char *g_stores_our_mmap;
static long long g_stores_prev_head;
//specific variable to load ring buffer
volatile sig_atomic_t g_loads_count_overflow_events=0;
static char *g_loads_our_mmap;
static long long g_loads_prev_head;

struct schedule_manager *g_shared_memory; //variable that stores the contents of the shared memory
int g_fd_shared_memory ; //file descriptor to shared memory

static int g_total_samples_mapped;
static int g_total_samples;

pthread_t g_sample_processor;


/*

Function Prototypes

*/

long long perf_mmap_read(void *, int , long long , int, int, long long , int , int *, int);
void close_monitor(int , siginfo_t *, void *); // handler to finish the execution
void store_handler(int, siginfo_t *, void *);
void load_handler(int, siginfo_t *, void *);
void setup_shared_memory(void); // open shared memory
int get_vector_index(long long, int *); //return correct index to update smaple_vector
int account_samples_to_allocations(void); // mapping samples to allocations
//float get_timestamp_diff_in_seconds(struct timespec , struct timespec );
int open_perf_clean(void); //disable event and deallocates mmap to buffer ring
int open_perf_stop(void); // disable allocation_statistics collect, don't clear the buffer
int open_perf_start(void); // enable allocation_statistics collect
int open_perf_setup(char *); //setup perf_event_open function to load and store only in CPU 0

/*

Function Implementations

*/


long long perf_mmap_read(void *our_mmap, \
                         int mmap_size, \
                         long long prev_head, \
                         int sample_type, \
                         int read_format, \
                         long long reg_mask, \
                         int g_quiet, \
                         int *events_read,\
                         int curr_ring_index){

	struct perf_event_mmap_page *control_page = our_mmap;
	long long head,offset;
	int i,size;
	long long bytesize,prev_head_wrap;
    
    int vector_index;
    int mem_level;
    int load=0;
    long long weight;
    
	unsigned char data[MMAP_DATA_SIZE * 4096];

    int tier_type;//0 to dram , 1 to pmem
    
	void *data_mmap=our_mmap+getpagesize();

	if (mmap_size==0) return 0;

	if (control_page==NULL) {
		fprintf(stderr,"ERROR mmap page NULL\n");
		return -1;
	}
	head=control_page->data_head;
	rmb(); /* Must always follow read of data_head */

	size=head-prev_head;

	if (g_debug) {
		printf("Head: %lld prev_head=%lld\n",head,prev_head);
		printf("%d new bytes\n",size);
	}
	bytesize=mmap_size*getpagesize();
    //fprintf(stderr, "[monitor] bytesize:%ld \n", bytesize);

	if (size>bytesize) {
		printf("error!  we overflowed the mmap buffer %d>%lld bytes\n",	size,bytesize);
	}
    //fprintf(stderr, "[monitor] read 4\n");
    
	//data=malloc(bytesize);
    
	if (data==NULL) {
		return -1;
	}
	if (g_debug) {
		printf("Allocated %lld bytes at %p\n",bytesize,data);
	}

	prev_head_wrap=prev_head%bytesize;

	if (g_debug) {
		printf("Copying %lld bytes from (%p)+%lld to (%p)+%d\n",  bytesize-prev_head_wrap,data_mmap,prev_head_wrap,data,0);
	}
	memcpy(data,(unsigned char*)data_mmap + prev_head_wrap,	bytesize-prev_head_wrap);

	if (g_debug) {
		printf("Copying %lld bytes from %d to %lld\n", prev_head_wrap,0,bytesize-prev_head_wrap);
	}
	memcpy(data+(bytesize-prev_head_wrap),(unsigned char *)data_mmap, prev_head_wrap);

	struct perf_event_header *event;


	offset=0;
	if (events_read) *events_read=0;
    
    g_total_samples = 0;
    g_total_samples_mapped = 0;
    
    //fprintf(stderr, "[monitor binary ] size:%d\n", size);
	while(offset<size) {

		if (g_debug) printf("Offset %lld Size %d\n",offset,size);
		event = ( struct perf_event_header * ) & data[offset];

		offset+=8; /* skip header */

		/***********************/
		/* Print event Details */
		/***********************/
		switch(event->type) {
                
				case PERF_RECORD_SAMPLE:
					if (sample_type & PERF_SAMPLE_IP) {
						long long ip;
						memcpy(&ip,&data[offset],sizeof(long long));
						if (!g_quiet) printf("\tPERF_SAMPLE_IP, IP: %llx\n",ip);
						offset+=8;
					}
					if (sample_type & PERF_SAMPLE_TID) {
						int pid, tid;
						memcpy(&pid,&data[offset],sizeof(int));
						memcpy(&tid,&data[offset+4],sizeof(int));
						

						if (!g_quiet) {
							printf("pid:%d,tid:%d,",pid,tid);
						}
						offset+=8;
					}
					if (sample_type & PERF_SAMPLE_TIME) {
						long long time;
						memcpy(&time,&data[offset],sizeof(long long));
						if (!g_quiet) printf("%lld,",time);
						offset+=8;
					}
					if (sample_type & PERF_SAMPLE_ADDR) {
						long long addr;
						memcpy(&addr,&data[offset],sizeof(long long));
						if (!g_quiet) printf("0x%llx,",addr);
						offset+=8;
                        //fprintf(stderr, "0x%llx, ",addr);
                        
                        
                        vector_index = get_vector_index(addr, &tier_type);
                        //fprintf(stderr, "vector_index:%d tier_type:%d\n", vector_index, tier_type);
                        //if(vector_index == -1) //means allocation_statistics is not from our application
                        //    break;
                        //else
                        //    g_total_samples_mapped ++;
                        
					}
					if (sample_type & PERF_SAMPLE_WEIGHT) {
						memcpy(&weight,&data[offset],sizeof(long long));
						//fprintf(stderr,"\t %lld,\n",weight);
						offset+=8;
                        
						//if (!g_quiet) printf("\n");
					}
					if (sample_type & PERF_SAMPLE_DATA_SRC) {
						long long src;

						memcpy(&src,&data[offset],sizeof(long long));
						//if (!g_quiet) printf("\tPERF_SAMPLE_DATA_SRC, Raw: %llx\n",src);
						offset+=8;
                        mem_level = -1;
                        

						if (vector_index != -1) {
                            //fprintf(stderr, "INICIO \t");
                            g_total_samples_mapped ++;
							//if (src!=0) printf("\t\t");
							if (src & (PERF_MEM_OP_NA<<PERF_MEM_OP_SHIFT))
								D fprintf(stderr,"Op Not available ");
                            if (src & (PERF_MEM_OP_LOAD<<PERF_MEM_OP_SHIFT)){
                                D fprintf(stderr,"Load_");
                                //fprintf(stderr, "Load in index:%d\n", vector_index);
                                load = 1;
                            }
                            if (src & (PERF_MEM_OP_STORE<<PERF_MEM_OP_SHIFT)){
								D fprintf(stderr,"Store_");
                                //fprintf(stderr, "Store count in index:%d\n", vector_index);
                                g_shared_memory->tier[tier_type].obj_vector[vector_index].ring.stores_count[curr_ring_index]++;
                            }
                            if (src & (PERF_MEM_LVL_L1<<PERF_MEM_LVL_SHIFT)){
								D fprintf(stderr,"L1,");
                                g_shared_memory->tier[tier_type].obj_vector[vector_index].ring.loads_count[curr_ring_index][0]++;
                                mem_level = 0;
                            }
                            if (src & (PERF_MEM_LVL_LFB<<PERF_MEM_LVL_SHIFT)){
								D fprintf(stderr,"LFB,");
                                g_shared_memory->tier[tier_type].obj_vector[vector_index].ring.loads_count[curr_ring_index][1]++;
                                mem_level = 1;
                            }
                            if (src & (PERF_MEM_LVL_L2<<PERF_MEM_LVL_SHIFT)){
								D fprintf(stderr, "L2,");
                                g_shared_memory->tier[tier_type].obj_vector[vector_index].ring.loads_count[curr_ring_index][2]++;
                                mem_level = 2;
                            }
                            if (src & (PERF_MEM_LVL_L3<<PERF_MEM_LVL_SHIFT)){
								D fprintf(stderr, "L3,");
                                g_shared_memory->tier[tier_type].obj_vector[vector_index].ring.loads_count[curr_ring_index][3]++;
                                mem_level = 3;
                            }
                            if (src & (PERF_MEM_LVL_LOC_RAM<<PERF_MEM_LVL_SHIFT)){
								D fprintf(stderr, "DRAM,");
                                g_shared_memory->tier[tier_type].obj_vector[vector_index].ring.loads_count[curr_ring_index][4]++;
                                mem_level = 4;
                            }
                            if(load == 1 && mem_level != -1){
                                D fprintf(stderr,"Weight=%lld, ",weight);
                                g_shared_memory->tier[tier_type].obj_vector[vector_index].ring.sum_latency_cost[curr_ring_index][mem_level] += weight;
                                load = 0;
                            }
                            if (src & (PERF_MEM_TLB_HIT<<PERF_MEM_TLB_SHIFT)){
								D fprintf(stderr,"TLB_Hit ");
                                g_shared_memory->tier[tier_type].obj_vector[vector_index].ring.TLB_hit[curr_ring_index][mem_level]++;
                            }
                            if (src & (PERF_MEM_TLB_MISS<<PERF_MEM_TLB_SHIFT)){
								D fprintf(stderr,"TLB_Miss ");
                                g_shared_memory->tier[tier_type].obj_vector[vector_index].ring.TLB_miss[curr_ring_index][mem_level]++;
                            }
                            D fprintf(stderr,"\n");
						}
						
					}
					break;

				default:
					//if (!g_quiet) printf("\tUnknown type %d\n",event->type);
					
					offset=size;
					//fprintf(stderr,"\tUnknown type %d\n",event->type);

		}
        
		if (events_read) (*events_read)++;

		g_total_samples++;		

	}

	control_page->data_tail=head;

	//free(data);

	return head;

}
void close_monitor(int signum, siginfo_t *info, void *uc){
    fprintf(stderr, "[monitor], [sample_processor] closed! \n");
    open_perf_clean();
    g_running = 0;
}
void store_handler(int signum, siginfo_t *info, void *uc) {
	int ret;
    
	if(info->si_code == POLL_HUP){
		int fd = info->si_fd;
        
		g_stores_count_overflow_events++;

		ret=ioctl(fd, PERF_EVENT_IOC_REFRESH, NUMBER_SAMPLE_OVERFLOW);
		(void) ret;
	}
}
void load_handler(int signum, siginfo_t *info, void *uc) {
    int ret;
    
    if(info->si_code == POLL_HUP){
        int fd = info->si_fd;

        g_loads_count_overflow_events++;

        ret=ioctl(fd, PERF_EVENT_IOC_REFRESH, NUMBER_SAMPLE_OVERFLOW);
        (void) ret;
    }
}
void setup_shared_memory(void){
    g_fd_shared_memory = shm_open(STORAGE_ID, O_RDWR | O_CREAT | O_EXCL, 0660);
    g_fd_shared_memory = shm_open(STORAGE_ID, O_RDWR, 0);
    g_shared_memory = mmap(0,sizeof(struct schedule_manager),PROT_READ|PROT_WRITE,MAP_SHARED,g_fd_shared_memory,0);

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
int account_samples_to_allocations(void){
    int i, j;
    int total_load_samples, total_load_mapped;
    int total_store_samples, total_store_mapped;
    struct timespec start, end;
    long long head_loads, head_stores;
    static long int ring_index = 0;
    int curr_ring_index;

    curr_ring_index = ring_index % RING_BUFFER_SIZE;
    
    pthread_mutex_lock(&g_shared_memory->global_mutex);

    clock_gettime(CLOCK_REALTIME, &start);
    //g_shared_memory->tier[0].obj_vector[i].ring.current_ring_index = curr_ring_index;
    
    
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
    
    g_loads_prev_head=perf_mmap_read(g_loads_our_mmap,MMAP_DATA_SIZE,g_loads_prev_head, g_sample_type,0,0, g_quiet,NULL, curr_ring_index);
    //head_loads = perf_mmap_read(g_loads_our_mmap,MMAP_DATA_SIZE,g_loads_prev_head, g_sample_type,0,0, g_quiet,NULL);
    total_load_samples = g_total_samples;
    total_load_mapped = g_total_samples_mapped;
    
    g_stores_prev_head=perf_mmap_read(g_stores_our_mmap,MMAP_DATA_SIZE,g_stores_prev_head, g_sample_type,0,0, g_quiet,NULL, curr_ring_index);
    //head_stores = perf_mmap_read(g_stores_our_mmap,MMAP_DATA_SIZE,g_stores_prev_head, g_sample_type,0,0, g_quiet,NULL);
    total_store_samples = g_total_samples;
    total_store_mapped = g_total_samples_mapped;
    
    clock_gettime(CLOCK_REALTIME, &end);
    
    
   
    for(i=0; i< g_shared_memory->tier[1].num_obj; i++){
            g_shared_memory->tier[1].obj_vector[i].ring.stores_count[curr_ring_index] = 0;
            fprintf(stderr, "PMEM object[%d]\n",i);
            for(j=4 ; j< MEM_LEVELS; j++){
                fprintf(stderr, "Lat Level [%d] %lu\t",j, g_shared_memory->tier[1].obj_vector[i].ring.sum_latency_cost[curr_ring_index][j]);
                fprintf(stderr, "Loads Level [%d] %lu\t",j, g_shared_memory->tier[1].obj_vector[i].ring.loads_count[curr_ring_index][j]);
                fprintf(stderr, "TLB hit Level [%d] %lu\t",j, g_shared_memory->tier[1].obj_vector[i].ring.TLB_hit[curr_ring_index][j]);
                fprintf(stderr, "TLB miss Level [%d] %lu\n",j, g_shared_memory->tier[1].obj_vector[i].ring.TLB_miss[curr_ring_index][j]);
            }
    }
    fprintf(stderr, "-----------------\n");
    
    D fprintf(stderr, "[monitor]  %d, %d, %f, %5d, %5d, %.1f, %.1f\n",\
    				g_loads_count_overflow_events,\
    				g_stores_count_overflow_events,\
                    get_timestamp_diff_in_seconds(start, end),\
    				total_load_samples, \
    				total_store_samples,\
    				((float)total_load_mapped/total_load_samples),\
    				((float)total_store_mapped/total_store_samples));
    
    /*
    for(i=0 ;i< g_shared_memory->tier[0].num_obj; i++){
        if(g_shared_memory->tier[0].obj_vector[i].ring.loads_count[curr_ring_index][4] != 0){
            fprintf(stderr, "\t DRAM Start_addr:%p index:%d LLC miss:%d \n", \
            					g_shared_memory->tier[0].obj_vector[i].start_addr,\
            					i,\
            					g_shared_memory->tier[0].obj_vector[i].ring.loads_count[curr_ring_index][4]);
        }
    }
    
    for(i=0 ;i< g_shared_memory->tier[1].num_obj; i++){
        if(g_shared_memory->tier[1].obj_vector[i].ring.loads_count[4] != 0){
            fprintf(stderr, "\t PMEM Start_addr:0x%p index:%d LLC miss:%d \n", g_shared_memory->tier[1].obj_vector[i].start_addr,i, g_shared_memory->tier[1].obj_vector[i].ring.loads_count[4]);
        }
    }
    */

    pthread_mutex_unlock(&g_shared_memory->global_mutex);
    
    g_loads_count_overflow_events = 0;
    g_stores_count_overflow_events = 0;
    
    //account_samples_to_allocations_binary_search(g_loads_our_mmap,g_loads_prev_head,g_stores_our_mmap,g_stores_prev_head);
    //g_loads_prev_head = head_loads;
    //g_stores_prev_head = head_stores;
    
    ring_index ++;
    
}
int open_perf_clean(void){
    
    //munmap(g_loads_our_mmap,g_mmap_pages*getpagesize());
    //munmap(g_stores_our_mmap,g_mmap_pages*getpagesize());

    close(g_fd[0]);
    close(g_fd[1]);
}
int open_perf_stop(void){
    int ret;

    ioctl(g_fd[0], PERF_EVENT_IOC_DISABLE,0);
    ioctl(g_fd[1], PERF_EVENT_IOC_DISABLE,0);
    
}
int open_perf_start(void){
    int ret;
   
    ioctl(g_fd[0], PERF_EVENT_IOC_RESET, 0);
    ioctl(g_fd[0], PERF_EVENT_IOC_REFRESH, NUMBER_SAMPLE_OVERFLOW);
    ret = ioctl(g_fd[0], PERF_EVENT_IOC_ENABLE,0);

    if (ret<0) {
        if (!g_quiet) {
            fprintf(stderr,"[monitor] Error %d %s\n", errno, strerror(errno));
        }
        return -1;
    }
    
    ioctl(g_fd[1], PERF_EVENT_IOC_RESET, 0);
    ioctl(g_fd[1], PERF_EVENT_IOC_REFRESH, NUMBER_SAMPLE_OVERFLOW);
    ret = ioctl(g_fd[1], PERF_EVENT_IOC_ENABLE,0);

    if (ret<0) {
        if (!g_quiet) {
            fprintf(stderr,"[monitor] Error %d %s\n", errno, strerror(errno));
        }
        return -1;
    }
    
	return 0;

}
int open_perf_setup(char *event) {

    int ret;
	int fd;	
	
	struct perf_event_attr pe;
    pfm_perf_encode_arg_t arg;
	struct sigaction sa;
	
	memset(&sa, 0, sizeof(struct sigaction));
	memset(&pe,0,sizeof(struct perf_event_attr));
    memset(&arg, 0, sizeof(pfm_perf_encode_arg_t));
    pe.size = sizeof(struct perf_event_attr);
    arg.size = sizeof(pfm_perf_encode_arg_t);

    ret = pfm_initialize();
    if (ret != PFM_SUCCESS) {
        return -1;  //ERROR_PFM;
    }
	
    arg.attr = &pe;
    char *fstr;
    arg.fstr = &fstr;
    
	if(strcmp(event,"loads") == 0){
            sa.sa_sigaction = load_handler;
       		sa.sa_flags = SA_SIGINFO;
       		g_mmap_pages=1+MMAP_DATA_SIZE;
       		ret = pfm_get_os_event_encoding("MEM_TRANS_RETIRED:LOAD_LATENCY:ldlat=3", PFM_PLM3, PFM_OS_PERF_EVENT, &arg);
       		if (ret != PFM_SUCCESS){
           		fprintf(stderr, "cannot get encoding %s\n", pfm_strerror(ret));
           		exit(-1);
       		}
       		if (sigaction(SIGUSR1, &sa, NULL) < 0) {
         		fprintf(stderr,"Error setting up signal handler\n");
         		exit(1);
       		}
	}else if(strcmp(event,"stores") == 0){
       		sa.sa_sigaction = store_handler;
       		sa.sa_flags = SA_SIGINFO;
            g_mmap_pages=1+MMAP_DATA_SIZE;
	        ret = pfm_get_os_event_encoding("MEM_INST_RETIRED:ALL_STORES", PFM_PLM3, PFM_OS_PERF_EVENT, &arg);
       		if (ret != PFM_SUCCESS){
      		     fprintf(stderr, "cannot get encoding %s\n", pfm_strerror(ret));
           		exit(-1);
       		}
            if (sigaction(SIGUSR2, &sa, NULL) < 0) {
                fprintf(stderr,"Error setting up signal handler\n");
                exit(1);
            }
	}else {
        return -1;
    }
	
	
	pe.precise_ip=2;    	
   	pe.type=PERF_TYPE_RAW;    
    pe.sample_period=SAMPLE_FREQUENCY;
    pe.sample_type= g_sample_type ;    
    pe.sample_stack_user=getpagesize();
    
    pe.disabled=1; //do not start sampling
    pe.exclude_kernel=1; //exclude kernel sampling
    pe.exclude_hv=1;//do not sampling hypervisor
    pe.exclude_user=0; //monitore user events
	pe.mmap=1;
    pe.task=1;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,1,0)
    pe.use_clockid=1;
    pe.clockid = CLOCK_MONOTONIC_RAW;
#endif

    //pid_t pid = 0; //measure the calling process
    int cpu = -1; //measure any cpu
    int group_fd = -1; //no event grouping - creates a new group
    unsigned long flags = 0; 

    fd=syscall(__NR_perf_event_open,&pe, -1, 0, group_fd, flags);
	if (fd<0) {
		if (!g_quiet) {
			fprintf(stderr,"Problem opening leader %s\n",strerror(errno));
		}
		return -1;
	}
	
	if(strcmp(event,"stores") == 0){
       g_stores_our_mmap=mmap(NULL, g_mmap_pages*getpagesize(), PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    }
    else if (strcmp(event,"loads") == 0){
       g_loads_our_mmap=mmap(NULL, g_mmap_pages*getpagesize(), PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    }
    
    fcntl(fd, F_SETFL, O_RDWR|O_NONBLOCK|O_ASYNC);
	if(strcmp(event,"loads") == 0)
	   fcntl(fd, F_SETSIG, SIGUSR1);
	else if (strcmp(event,"stores") == 0)   
	   fcntl(fd, F_SETSIG, SIGUSR2);
	
	fcntl(fd, F_SETOWN,getpid());
    
    return fd;
}
int main(int argc, char **argv) {
    struct timespec start, end;
    double elapsedTime;
	struct sigaction sa;
    
    clock_gettime(CLOCK_REALTIME, &start);
    //fprintf(stderr, "[monitor] Start: %ld.%ld\n",start.tv_sec,start.tv_nsec);
    
    sa.sa_sigaction = close_monitor;
    sa.sa_flags = SA_SIGINFO;
    
    if (sigaction(SIGPROF, &sa, NULL) < 0) { //SIGPROF Profiling timer expired
         fprintf(stderr,"Error setting up signal handler\n");
         exit(1);
    }
    
    setup_shared_memory();
    //setup_shared_memory_binary_search();

    g_fd[0] = open_perf_setup("loads");
    g_fd[1] = open_perf_setup("stores");
    
    
    if (open_perf_start()){
          fprintf(stderr, "[monitor] open_perf_start() Error!!\n");
          return -1;
    }
    
    pthread_create(&g_sample_processor, NULL, thread_sample_processor, g_shared_memory);
    
    while(g_running){
        
    	if(g_stores_count_overflow_events || g_loads_count_overflow_events){
				open_perf_stop();
                account_samples_to_allocations();
                
                D fprintf(stderr, "-------------------------------------------------\n");
                open_perf_start();
    	}
        
    	//sleep(3);
        //open_perf_stop();
    }
	clock_gettime(CLOCK_REALTIME, &end);
    //fprintf(stderr, "[monitor] End: %ld.%ld\n",end.tv_sec,end.tv_nsec);
    
    long seconds = end.tv_sec - start.tv_sec;
    long ns = end.tv_nsec - start.tv_nsec;
    if (start.tv_nsec > end.tv_nsec) { // clock underflow
        --seconds;
        ns += 1000000000;
    }
    
    uint64_t delta_us = (end.tv_sec - start.tv_sec) * 1000000 + (end.tv_nsec - start.tv_nsec) / 1000; 
    //fprintf(stderr, "[monitor] ElapsedTime: %.2lf seconds \n",(float)delta_us/1000000);

    
	return 0;
}
