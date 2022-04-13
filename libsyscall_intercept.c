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

#include "recorder.h"

//tier_manager_t g_tier_manager;
pthread_t thread_test;


static int
hook(long syscall_number,
			long arg0, long arg1,
			long arg2, long arg3,
			long arg4, long arg5,
			long *result)
{
		
	if (syscall_number == SYS_mmap) {

		*result = syscall_no_intercept(syscall_number, arg0, arg1, arg2, arg3, arg4, arg5);		       
		fprintf(stderr, "mmap\n");
		//insert_object((int)getpid(), *result, (long)arg1);
		
		return 0;
	}else if(syscall_number == SYS_munmap){
        
		*result = syscall_no_intercept(syscall_number, arg0, arg1, arg2, arg3, arg4, arg5);
		fprintf(stderr, "munmap\n");
        //remove_object((int)getpid(), arg0, arg1);
        
		return 0;
	}else {
		return 1;
	}
}

void *thread_test_function(void * _args)
{
	while(1){
		fprintf(stderr, "Dentro da thread de test\n");
		sleep(10);
	}
	
}

static __attribute__((constructor)) void
init(int argc, char * argv[])
{
	setvbuf(stdout, NULL, _IONBF, 0);  //avoid buffer from printf
	
	intercept_hook_point = hook;
	
    pthread_create(&thread_test, NULL, thread_test_function, NULL);
	
}

