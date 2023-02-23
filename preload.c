/*
 * Hook main() using LD_PRELOAD, because why not?
 * Obviously, this code is not portable. Use at your own risk.
 *
 * Compile using 'gcc hax.c -o hax.so -fPIC -shared -ldl'
 * Then run your program as 'LD_PRELOAD=$PWD/hax.so ./a.out'
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <pthread.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <numaif.h>
#include <numa.h>
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include "recorder.h"
#define STORAGE_ID "MY_SHARED_MEMORY"

pthread_t thread_mbind;
static struct timespec g_start, g_end;
FILE *g_fp;
int g_pipe_read_fd;
int g_pipe_write_fd;
static int g_running = 1;
static int g_num_nodes_available;
extern int errno;


static void __attribute__ ((constructor)) init_lib(void);
//static void __attribute__((destructor)) exit_lib(void);

void *thread_manager_mbind(void * _args);

int static guard(int ret, char *err)
{
    if (ret == -1)
    {
        perror(err);
        return -1;
    }
    return ret;
}
double static get_my_timestamp(){
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
void set_number_of_nodes_availables(void)
{
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

void static preload_open_pipes(void)
{
    char FIFO_PATH_MIGRATION[50] ;
    char FIFO_PATH_MIGRATION_ERROR[50] ;

    sprintf(FIFO_PATH_MIGRATION, "migration.pipe");
    sprintf(FIFO_PATH_MIGRATION_ERROR, "migration_error.pipe", getpid());

    g_pipe_read_fd = guard(open(FIFO_PATH_MIGRATION, O_RDONLY), "[preload] Could not open pipe MIGRATION for reading");
    g_pipe_write_fd = guard(open(FIFO_PATH_MIGRATION_ERROR, O_WRONLY), "[preload] Could not open pipe MIGRATION_ERROR for writing");
}

/*
 O único objetivo de uma shared memory nesse escalonador é evitar que novas instancias (novos processos)
 gerados a partir do PID monitorado, crie novos processos e por consequência instancie novamente
 thread_mbind.
 */
void init_lib(void)
{
   int fd=shm_open(STORAGE_ID, O_RDWR | O_CREAT | O_EXCL, 0660);
   if(fd != -1)
   {
       preload_open_pipes();
       pthread_create(&thread_mbind, NULL, thread_manager_mbind, NULL);
   }
   //else{
       //D fprintf(stderr, "[preload] PID: %d trying to create thread mbind. Has shared memory been removed?\n", getpid());
   //}
}


/*
 query_status_memory_pages uses the move_pages syscall in query mode (with null nodes) to check the page's status
 of pages ina specifc interval.
 
 query_status_memory_pages it returns the id of the NUMA node of the pointer in status.
 
 migrate_pages()  moves  all pages of the process pid that are in memory nodes old_nodes to the memory nodes in
 new_nodes.  Pages not located in any node in old_nodes will not be migrated.
 
 */
static int query_status_memory_pages(int pid, unsigned long int addr, unsigned long int size, float *status_memory_pages)
{
    unsigned node = 0;
    // size must be page-aligned
    size_t pagesize = getpagesize();
    if((size % pagesize) != 0){
        for(int i = 0; i < g_num_nodes_available + 1; i++){
            status_memory_pages[i] = -1;
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
        fprintf(stderr, "[preload - numa_move_pages() syscall] error code: %d, %p\n", errno, addr);
        perror("error description:");
    }

    for (int i = 0; i < page_count; i++) {
        if(status[i] >= 0 && status[i] < g_num_nodes_available){
            status_memory_pages[status[i]]++;//0,1,2
        }else{
            status_memory_pages[g_num_nodes_available]++;
        }
    }

    for(int i = 0; i < g_num_nodes_available + 1; i++){
        status_memory_pages[i] = (float)status_memory_pages[i]/page_count;
    }
    return 0;
}

void execute_mbind(data_bind_t data)
{
    static struct timespec timestamp;
    char cmd[100];

    if (mbind((void *)data.start_addr,
              data.size,
              //MPOL_BIND, &data.nodemask_target_node,  //avoiding to invoke the OOM killer
              MPOL_PREFERRED, &data.nodemask_target_node,
              64,
              //MPOL_MF_MOVE) == -1)
              data.flag) == -1)
    {
        write(g_pipe_write_fd, &data, sizeof(data_bind_t));

        if(data.type != INITIAL_DATAPLACEMENT){
            sprintf(cmd, "echo %lf, %d, %p, %ld, %lu, %d, %d >> bind_error.txt", \
                    get_my_timestamp(), \
                    data.obj_index, \
                    data.start_addr, \
                    data.size, \
                    data.nodemask_target_node, \
                    errno, \
                    data.type);
            system(cmd);
        }
    }
}

void *thread_manager_mbind(void * _args){
    int i;
    char filename[50];
    float *status_memory_pages_before = NULL;//The last vector position is to save unmapped pages
    float *status_memory_pages_after = NULL;//The last vector position is to save unmapped pages

    set_number_of_nodes_availables();

    status_memory_pages_before = malloc((g_num_nodes_available + 1) * sizeof(float));
    status_memory_pages_after = malloc((g_num_nodes_available + 1) * sizeof(float));

    if(status_memory_pages_before == NULL || status_memory_pages_after==NULL){
        fprintf(stderr, "Error during malloc to status_memory_pages\n");
        exit(-1);
    }

    //sprintf(filename, "/tmp/preload_migration_cost.txt");
    sprintf(filename, "preload_migration_cost.txt");
    g_fp = fopen(filename, "w");
    if(g_fp == NULL){
        fprintf(stderr, "Error while trying to open Migration cost\n");
        perror(filename);
    }

    //open_pipes();

    while(g_running){
        data_bind_t buf;

        ssize_t num_read = guard(read(g_pipe_read_fd, &buf, sizeof(data_bind_t)), "Could not read from pipe");
        if (num_read == 0)
        {
           fprintf(stderr, "[preload] Piper to read is empty! \n");
        }
        else
        {
#ifdef DEBUG
           fprintf(g_fp, "%lf, %d, %p, %ld, %d, ", \
                   get_my_timestamp() , \
                   buf.obj_index, \
                   buf.start_addr, \
                   buf.size, \
                   buf.nodemask_target_node);

           for(int i=0 ;i< g_num_nodes_available + 1; i++){
               status_memory_pages_before[i] = 0;
           }
           //Get page state before migration
           query_status_memory_pages(getpid(), buf.start_addr, buf.size, status_memory_pages_before);
           fprintf(g_fp, "[%.2f:",status_memory_pages_before[0]);
           for(i=1; i<g_num_nodes_available; i++)
           {
               fprintf(g_fp, "%.2f:",status_memory_pages_before[i]);
           }
           fprintf(g_fp, "%.2f],",status_memory_pages_before[i]);

           clock_gettime(CLOCK_REALTIME, &g_start);
#endif
           //fprintf(g_fp, "[preload] %lf, %d, %p, %ld\n", get_my_timestamp() , buf.obj_index, buf.start_addr, buf.size);
           execute_mbind(buf);
#ifdef DEBUG
           clock_gettime(CLOCK_REALTIME, &g_end);

           uint64_t delta_us = (g_end.tv_sec - g_start.tv_sec) * 1000000 + (g_end.tv_nsec - g_start.tv_nsec) / 1000;

           fprintf(g_fp, " %.2lf, ",delta_us/1000.0);

           for(i=0 ;i< g_num_nodes_available + 1; i++){
               status_memory_pages_after[i] = 0;
           }

           //Get page state after migration
           query_status_memory_pages(getpid(), buf.start_addr, buf.size, status_memory_pages_after);
           fprintf(g_fp, "[%.2f:",status_memory_pages_after[0]);
           for(i=1; i<g_num_nodes_available; i++)
           {
               fprintf(g_fp, "%.2f:",status_memory_pages_after[i]);
           }
           fprintf(g_fp, "%.2f], ",status_memory_pages_after[i]);
           fprintf(g_fp, "%d\n",buf.type);
#endif
        }
    }
}
static int (*main_orig)(int, char **, char **);
int main_hook(int argc, char **argv, char **envp)
{
    int ret = main_orig(argc, argv, envp);
    g_running = 0;
    return ret;
}
int __libc_start_main(
    int (*main)(int, char **, char **),
    int argc,
    char **argv,
    int (*init)(int, char **, char **),
    void (*fini)(void),
    void (*rtld_fini)(void),
    void *stack_end)
{
    /* Save the real main function address */
    main_orig = main;

    /* Find the real __libc_start_main()... */
    typeof(&__libc_start_main) orig = dlsym(RTLD_NEXT, "__libc_start_main");

    /* ... and call it with our custom main function */
    return orig(main_hook, argc, argv, init, fini, rtld_fini, stack_end);
}
