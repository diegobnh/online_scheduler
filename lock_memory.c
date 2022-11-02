#include<unistd.h>
#include<stdio.h>
#include<time.h>
#include<stdlib.h>
#include<sys/mman.h>  //this is to mlock
#include<signal.h>
#include<inttypes.h>
#define GB 1048576000UL //1000 x 1024 x 1024


/*
gcc -o lock_memory lock_memory.c
numactl --membind=0 ./lock_memory 7
*/

int g_loop_flag = 1;

//just send kill -10 PID
//kill -10 $(pidof lock_memory)
void stop_lock_memory(int signum, siginfo_t *info, void *uc){
    //printf("Finishing..\n");
    munlockall();
    g_loop_flag = 0;
}

int main(int argc, char *argv[])
{
    if(argv[1] == NULL)
    {
        printf("Miss argument! eg: ./lock_memory 7\n");
    }

    struct sigaction sa;
    sa.sa_sigaction = stop_lock_memory;
    sa.sa_flags = SIGUSR1;

    int size_in_gb;
    sscanf(argv[1], "%d", &size_in_gb);

    uint64_t alloc_size = GB * size_in_gb;

    char* memory = malloc (alloc_size);
    mlock (memory, alloc_size);

    if (sigaction(SIGUSR1, &sa, NULL) < 0) { //SIGPROF Profiling timer expired
        fprintf(stderr,"Error setting up signal handler\n");
        return 1;
    }

    //mlock doesn't reserve physical memory for the calling process because the pages may be copy-on-write
    size_t i;
    size_t page_size = getpagesize ();
    for (i = 0; i < alloc_size; i += page_size)
        memory[i] = 0;

    while(g_loop_flag){
        sleep(10);
    }

    //printf("Finished!\n");
    return 0;
}
