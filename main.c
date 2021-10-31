//
//  main.c
//  
//
//  Created by diego moura on 06/02/21.
//
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <time.h>
#include <sys/types.h>
#include <stdint.h>
#include <sys/wait.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>

#include "monitor.h"

#define CPUS_PER_APPLICATION 1  //max value is 6
#define NUMBER_OF_APPLICATIONS 1
#define STR_SIZE 1024 //STR_SIZE doesn't have to be a constant


struct applications{
    char cmd[NUMBER_OF_APPLICATIONS][STR_SIZE]; //Partial command - necessary to execvp function
    char args[NUMBER_OF_APPLICATIONS][STR_SIZE] ; //Complete command
}g_application_command;


/*

Function Prototypes

*/
char **format_args_to_execvp(char *);
bool spawn_app(char* cmd, char* args, int );
void start_schedule(void);
void initialize_variables();

/*

Function Implementations

*/
char **format_args_to_execvp(char *input)
{
    char **command = malloc(8 * sizeof(char *));
    char *separator = " ";
    char *parsed;
    int index = 0;

    parsed = strtok(input, separator);
    while (parsed != NULL) {
        command[index] = parsed;
        index++;

        parsed = strtok(NULL, separator);
    }

    command[index] = NULL;
    return command;
}
bool spawn_app(char* cmd, char* args, int cpu_start)
{
    int i,j;
    int pid = fork();
    if(pid == -1)
    {
        perror("scheduler: failed to fork scheduled application");
        return false;
    }
    else if(pid == 0)
    {
        char **input_execvp = format_args_to_execvp(args);
        
        execvp(cmd, input_execvp);
        perror("scheduler: execvp failed");
        return false;
    }
    else
    {
        //char cmd[256];
        //sprintf(cmd, "taskset -cp %s %d",cpu_affinity, pid);
        //fprintf(stderr,"[main] Spawn app :%s PID:%d\n",args, pid);
        //system(cmd);
        
        cpu_set_t  mask;
        CPU_ZERO(&mask);
   
        for(i=cpu_start; i<cpu_start + (2*CPUS_PER_APPLICATION) ;i+=2){
            CPU_SET(i, &mask);
        }

        if (sched_setaffinity(pid,sizeof(cpu_set_t),&mask) == -1){
           perror("sched_setaffinity");
        }
        return true;
    }
}
void start_schedule(void)
{
    int i;
    /*
     We have 36 cores, 18 per socket
     We will use 6 cores per app
     app1 - cores 0,2,4,6,8,10
     app2 - cores 12,14,16,18,20,22
     app3 - cores 24,26,28,30,32,34
     */
    int cpu_affinity[3] = {0, 12, 24};
    
    
    for( i=0; i<NUMBER_OF_APPLICATIONS; i++ ) {
        if(!(spawn_app(g_application_command.cmd[i],g_application_command.args[i],cpu_affinity[i])))
           fprintf(stderr,"error when call spawn_app_on_dram()\n");
        //sleep(1);
    }       
}
void initialize_variables(void)
{
    int i;

    //Theses comands should be sorted by default based on degradation (DRAM->PMEM)
    char cmd [][STR_SIZE] = {"/scratch/gapbs/./cc",
                            "/scratch/gapbs/./bc",
                            "/scratch/gapbs/./cc"};

    char *args[] = {"/scratch/gapbs/./bc -f /scratch/gapbs/benchmark/graphs/kron.sg ",
					"/scratch/gapbs/./bc -f /scratch/gapbs/benchmark/graphs/urand.sg -n1",
					"/scratch/gapbs/./cc -f /scratch/gapbs/benchmark/graphs/kron.sg -k64"};


    for( i=0; i<NUMBER_OF_APPLICATIONS; i++ ) {
        strcpy(g_application_command.cmd[i], cmd[i]);
        strcpy(g_application_command.args[i], args[i]);
    }
}

void kill_monitor(){
    
    FILE * command = popen("pidof monitor","r");
    char buffer[50];
    fgets(buffer,50,command);
    int monitor_pid;
    sscanf(buffer, "%d", &monitor_pid);
    kill( monitor_pid, SIGPROF);
}

int main()
{
    int pid;
    int status;
    int count=0;
    
    initialize_variables();
    start_schedule();
    
    while(true){
        pid = wait(&status);
        count ++;
        //fprintf(stderr,"[main] Child PID:%d finished! \n",pid);
        if(count == NUMBER_OF_APPLICATIONS)
        {
            break;
        }
    }
    fprintf(stderr,"[main] closed! \n",getpid());
    
    kill_monitor();
    
    return 0;
}



