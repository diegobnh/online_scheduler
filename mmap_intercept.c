
#include <syscall.h>
#include <errno.h>
#include <stdio.h>
#include <execinfo.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#define _GNU_SOURCE
#include <pthread.h>
#define SIZE 4096
#include <sys/resource.h> 
#include <sys/types.h>
#include <stdint.h>
#include <numaif.h>
#include "/ihome/dmosse/dmoura/0_tools/syscall/syscall_intercept/include/libsyscall_intercept_hook_point.h"
#include <sys/mman.h>
#include <sys/signal.h>
#include <signal.h>

#include "recorder.h"
#include "monitor.h"
#include "actuator.h"

#define STORAGE_ID "SHM_TEST"

#define NODE_0_DRAM 0
#define NODE_0_PMEM 2
#define NODE_1_DRAM 1
#define NODE_1_PMEM 3

#define ROUND_ROBIN 1
#define RANDOM 2
#define FIRST_DRAM 3
#define FIRST_PMEM 4

#ifndef INIT_ALLOC
	fprintf(stderr, "INIT_ALLOC not defined\n");
#endif

#if !(INIT_ALLOC == 1 || INIT_ALLOC == 2 || INIT_ALLOC == 3 || INIT_ALLOC == 4)
   fprintf(stderr, "INIT_ALLOC value invalid\n");
#endif

//#define DEBUG
#ifdef DEBUG
  #define D if(1)
#else
  #define D if(0)
#endif

static int pid;
struct schedule_manager *shared_memory;
pthread_t actuator;

static void __attribute__ ((constructor)) init_lib(void);
static void __attribute__((destructor)) exit_lib(void);


void init_lib(void)
{
   int fd=shm_open(STORAGE_ID, O_RDWR | O_CREAT | O_EXCL, 0660);
   if(fd == -1)
   {
        //D fprintf(stderr,"\n\t Instance of shared-library already exist (Pid:%d)!\n\n", getpid());
        
        fd=shm_open(STORAGE_ID, O_RDWR, 0);
        shared_memory = mmap(0,sizeof(struct schedule_manager),PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);

        pthread_mutex_lock(&shared_memory->global_mutex);
        shared_memory->account_shared_library_instances += 1;
        
        pthread_mutex_unlock(&shared_memory->global_mutex);
   }//So, this else will run just one time, even if several process instantiate this shared library
   else
   {
        //D fprintf(stderr,"\n\t First instance of shared-library (Pid:%d)\n\n", getpid());
        ftruncate(fd,sizeof(struct schedule_manager)); // set the size
        shared_memory = mmap(0,sizeof(struct schedule_manager),PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
       
        initialize_recorder(shared_memory);
        
        pthread_mutexattr_init(&shared_memory->global_attr_mutex);
        pthread_mutexattr_setpshared(&shared_memory->global_attr_mutex, PTHREAD_PROCESS_SHARED);
        pthread_mutex_init(&shared_memory->global_mutex, &shared_memory->global_attr_mutex);
       
        pthread_mutex_lock(&shared_memory->global_mutex);
        shared_memory->account_shared_library_instances += 1;
        pthread_mutex_unlock(&shared_memory->global_mutex);
       
        pthread_create(&actuator, NULL, thread_actuator, shared_memory);

   }
}
void exit_lib(void)
{
   pthread_mutex_lock(&shared_memory->global_mutex);

   if (shared_memory->account_shared_library_instances == 1)
   {
      //fprintf(stderr,"[actuator] closed! \n");
      //D fprintf(stderr,"\n\t\t Exit_lib  Desalocating shared-memory. (Last Pid:%d)\n\n", getpid());
      //munmap(shared_memory,sizeof(struct schedule_manager));
      shm_unlink(STORAGE_ID);
      exit(1);
      
   }
   else
   {
      //D fprintf(stderr,"\n\t\t Exit_lib (Pid:%d)\n\n", getpid());
      shared_memory->account_shared_library_instances -= 1;
      //D fprintf(stderr,"\n\t\t Account_shared_library_instances:%d\n",shared_memory->account_shared_library_instances);
      pthread_mutex_unlock(&shared_memory->global_mutex);
   }
}


static int
hook(long syscall_number, long arg0, long arg1,	long arg2, long arg3, long arg4, long arg5,	long *result)
{
    int flag_dram_alloc = 0;  //if 1, means the allocations went to DRAM 
	int static mmap_id = 0;
	static int memory_index = 0;
	struct timespec ts;
	unsigned long nodemask;
    struct timespec start, end;
    uint64_t delta_us;
    long int mem_consumption;
		
	if (syscall_number == SYS_mmap) {
        
		*result = syscall_no_intercept(syscall_number, arg0, arg1, arg2, arg3, arg4, arg5);
        
		pthread_mutex_lock(&shared_memory->global_mutex);
        mem_consumption = shared_memory->tier[0].current_memory_consumption;
        pthread_mutex_unlock(&shared_memory->global_mutex);

        
#if INIT_ALLOC == ROUND_ROBIN 		    
        if(((memory_index ++) %2)){
#elif INIT_ALLOC == RANDOM
        if(rand() % 2){
#elif INIT_ALLOC == FIRST_DRAM
        if(1){
#elif INIT_ALLOC == FIRST_PMEM
        if(0){
#endif
		   if((unsigned long)arg1 + mem_consumption < MAXIMUM_DRAM_CAPACITY){
               nodemask = 1<<NODE_0_DRAM;
               
		       fprintf(stderr, "[mmap - dram] %p %llu\n", (void*)*result, (unsigned long)arg1);
           
		       if(mbind((void*)*result, (unsigned long)arg1, MPOL_BIND, &nodemask, 64, MPOL_MF_MOVE) == -1)
		       {
					fprintf(stderr,"Error during mbind:%d\n",errno);
					perror("Error description"); 
		       }else{
					insert_allocation_on_dram(shared_memory, (int)getpid(), *result, (unsigned long)arg1);
	 		        flag_dram_alloc = 1;
	 		        return 0;
		       }
           }else{
               flag_dram_alloc = 0;
           }
		}
		if(flag_dram_alloc == 0)
		{
           //nodemask = 1<<NODE_1_DRAM ;
           nodemask = 1<<NODE_0_PMEM;
		   fprintf(stderr, "[mmap - pmem] %p %llu\n", (void*)*result, (unsigned long)arg1);

		   if(mbind((void *)*result, (unsigned long)arg1, MPOL_BIND, &nodemask, 64, MPOL_MF_MOVE) == -1)
		   {
			  fprintf(stderr,"Error during mbind:%d\n",errno);
			  perror("Error description"); 
		   }
		   
   		   insert_allocation_on_pmem(shared_memory, (int)getpid(), *result, (unsigned long)arg1);
   		   return 0;
   		   
		}
         
        /*
        nodemask = 1<<NODE_0_DRAM;
        if((unsigned long)arg1 == 536875008){
            fprintf(stderr, "[mmap - dram] %p %llu\n", (void*)*result, (unsigned long)arg1);
            if(mbind((void*)*result, (unsigned long)arg1, MPOL_BIND, &nodemask, 64, MPOL_MF_MOVE) == -1)
            {
                 fprintf(stderr,"Error during mbind:%d\n",errno);
                 perror("Error description");
            }else{
                 insert_allocation_on_dram(shared_memory, (int)getpid(), *result, (unsigned long)arg1);
                  flag_dram_alloc = 1;
                  return 0;
            }
        }else{
            nodemask = 1<<NODE_0_PMEM;
            fprintf(stderr, "[mmap - pmem] %p %llu\n", (void*)*result, (unsigned long)arg1);

            if(mbind((void *)*result, (unsigned long)arg1, MPOL_BIND, &nodemask, 64, MPOL_MF_MOVE) == -1)
            {
               fprintf(stderr,"Error during mbind:%d\n",errno);
               perror("Error description");
            }
            insert_allocation_on_pmem(shared_memory, (int)getpid(), *result, (unsigned long)arg1);
            return 0;
        }
		*/
		
	}else if(syscall_number == SYS_munmap){
        
		*result = syscall_no_intercept(syscall_number, arg0, arg1, arg2, arg3, arg4, arg5);
        
        D fprintf(stderr, "[mUNmap] %p %ld\n", (void*)arg0, arg1);
        //if return 0 means can't find object on dram, try to find on pmem
        
        if (remove_allocation_on_dram(shared_memory, (int)getpid(),arg0, arg1) != 1){
            
            if(remove_allocation_on_pmem(shared_memory, (int)getpid(), arg0, arg1) != 1){
                //D fprintf(stderr,"\t[recorder] Not founded neither in DRAM nor in PMEM!\n");
            }
        }
        
		return 0;
	}else {
		return 1;
	}
}

static __attribute__((constructor)) void
init(int argc, char * argv[])
{
    //srand(time(NULL));
    srand(123);
	setvbuf(stdout, NULL, _IONBF, 0);  //avoid buffer from printf
	intercept_hook_point = hook;
}


