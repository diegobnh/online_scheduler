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

#define DEBUG
#ifdef DEBUG
  #define D if(1)
#else
  #define D if(0)
#endif

pthread_t thread_mbind;
static struct timespec g_start, g_end;
FILE *g_fp;
int g_pipe_read_fd;
int g_pipe_write_fd;
static int g_running = 1;
static int g_num_nodes_available;
extern int errno;
static char g_buf[256];
static char g_cmd[256];
static double g_timestamp;
static FILE *g_stream_file;

/*
typedef struct data_bind
{
    unsigned long start_addr;
    unsigned long size;
    unsigned long nodemask_target_node;
    int obj_index;
} data_bind_t;
*/
static void __attribute__ ((constructor)) init_lib(void);
//static void __attribute__((destructor)) exit_lib(void);

void *thread_manager_mbind(void * _args);

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
       D fprintf(stderr, "[preload] creating thread_mbind\n");
       pthread_create(&thread_mbind, NULL, thread_manager_mbind, NULL);
   }else{
       D fprintf(stderr, "[preload] PID: %d trying to create thread mbind. Has shared memory been removed?\n", getpid());
   }

   sprintf(g_cmd, "awk '{print $1}' /proc/uptime");
}


/*
 query_status_memory_pages uses the move_pages syscall in query mode (with null nodes) to check the page's status
 of pages ina specifc interval.
 
 query_status_memory_pages it returns the id of the NUMA node of the pointer in status.
 
 migrate_pages()  moves  all pages of the process pid that are in memory nodes old_nodes to the memory nodes in
 new_nodes.  Pages not located in any node in old_nodes will not be migrated.
 
 */
int query_status_memory_pages(int pid, unsigned long int addr, unsigned long int size, float *status_memory_pages)
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
        fprintf(stderr, "[preload - numa_move_pages] error code: %d\n", errno);
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
int guard(int ret, char *err)
{
    if (ret == -1)
    {
        perror(err);
        return -1;
    }
    return ret;
}
void execute_mbind(data_bind_t data)
{
    static int count = 0;
    static struct timespec timestamp;
    char cmd[100];

    if (mbind((void *)data.start_addr,
              data.size,
              //MPOL_BIND, &data.nodemask_target_node,  //avoiding to invoke the OOM killer
              MPOL_PREFERRED, &data.nodemask_target_node,
              64,
              MPOL_MF_MOVE) == -1)
    {
        write(g_pipe_write_fd, &data, sizeof(data_bind_t));

        if(data.type != INITIAL_DATAPLACEMENT){
            sprintf(cmd, "echo %lf, %d, %p, %ld, %lu, %d, %d >> bind_error.txt", \
                    g_timestamp, \
                    data.obj_index, \
                    data.start_addr, \
                    data.size, \
                    data.nodemask_target_node, \
                    errno, \
                    data.type);
            system(cmd);

            char cmd2[30];
            //sprintf(cmd2, "cat /proc/%d/numa_maps | grep bind > maps_%d.%d", getpid(), count, getpid());
            //sprintf(cmd2, "cat /proc/%d/maps > maps_%d.txt", getpid(), data.type);
            system(cmd2);
            count++;
        }
    }
}


void open_pipes(void)
{
    char FIFO_PATH_MIGRATION[50] ;
    char FIFO_PATH_MIGRATION_ERROR[50] ;

    sprintf(FIFO_PATH_MIGRATION, "migration_%d.pipe", getpid());
    sprintf(FIFO_PATH_MIGRATION_ERROR, "migration_error_%d.pipe", getpid());

    guard(mkfifo(FIFO_PATH_MIGRATION, 0777), "Could not create pipe");
    g_pipe_read_fd = guard(open(FIFO_PATH_MIGRATION, O_RDONLY), "[preload] Could not open pipe MIGRATION for reading");
    guard(mkfifo(FIFO_PATH_MIGRATION_ERROR, 0777), "Could not create pipe");
    g_pipe_write_fd = guard(open(FIFO_PATH_MIGRATION_ERROR, O_WRONLY), "[preload] Could not open pipe MIGRATION_ERROR for writing");
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

    open_pipes();

    while(g_running){
        data_bind_t buf;

        ssize_t num_read = guard(read(g_pipe_read_fd, &buf, sizeof(data_bind_t)), "Could not read from pipe");
        if (num_read == 0)
        {
           D fprintf(stderr, "[preload] Piper to read is empty! \n");
        }
        else
        {

           if (NULL == (g_stream_file = popen(g_cmd, "r"))) {
               perror("popen");
               exit(EXIT_FAILURE);
           }

           fgets(g_buf, sizeof(g_buf), g_stream_file);
           sscanf(g_buf, "%lf",& g_timestamp);

           fprintf(g_fp, "%lf, %d, %p, %ld, %d, ", \
                   g_timestamp , \
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
           execute_mbind(buf);
           clock_gettime(CLOCK_REALTIME, &g_end);

           if(buf.size == 1000001536){
               char cmd2[30];
               sprintf(cmd2, "cat /proc/%d/maps > maps_%d.txt", getpid(), buf.type);
               system(cmd2);
           }


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
