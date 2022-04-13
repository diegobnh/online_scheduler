#!/bin/bash
: '
Vários comandos aqui necessitam de sudo. Existem duas formas de executar esse script.
(1) A primeira é usando o comando sudo ./run_this_script.sh

Nesse caso todo os comandos que exigem a senha de sudo já estarão cobertos.
O que precisa ser feito apenas saber se o sudo tem permissão de escrita no diretório onde
estão esses arquivos. Para criar um diretório com essa epermissão basta executar:
mkdir -m 777 dirname

(2) A segunda opção é rodar o script sem o sudo.
Nesse caso, o primeiro comando que ele encontrar que vier precedido de sudo ele vai solicitar a senha.
Entretando, se o próximo comando  a ser executado do sudo for executado com um intervalo de tempo superior
ao tempo em que a senha do sudo fica ativa, um novo pedido de senha será feito o que fará o script ficar
parado esperando essa senha.

Para subir os arquivos da minha máquina sem levar a pasta result/
rsync *.h *.c *.sh -avz  -e ssh 'dmoura@h2p.crc.pitt.edu:/ihome/dmosse/dmoura/test/'

'

sudo sleep 3

#bind this script to run in only one socket . In our case, node 0!
export OMP_NUM_THREADS=18
export OMP_PLACES={0}:18:2
export OMP_PROC_BIND=true

#Disable address space layout randomization the
#echo 0 | sudo tee /proc/sys/kernel/randomize_va_space

sudo sysctl -w kernel.numa_balancing=2 >  /dev/null
sudo sh -c 'echo true > /sys/kernel/mm/numa/demotion_enabled'
sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches'

METRICS=("LLCM_PER_SIZE")
#METRICS=("ABS_LLCM" "LLCM_PER_SIZE" "ABS_TLB_MISS" "TLB_MISS_PER_SIZE" "ABS_WRITE" "WRITE_PER_SIZE" "ABS_LATENCY" "LATENCY_PER_SIZE")

mkdir -p results
rm -rf results/*

#Initial Dataplacement Options
#ROUND_ROBIN 1
#RANDOM 2
#FIRST_DRAM 3
#FIRST_PMEM 4

gcc -o delete_shared_memory delete_shared_memory.c -lrt
gcc -O2 -g -c recorder.c -lpthread;
gcc -O2 -I/include -g -c intercept_mmap.c -lpthread;
gcc -O1 -I/include -g -c monitor.c -lrt -lm -lpfm -lpthread -lperf;
gcc -O2 -g -c track_mapping.c -DMETRIC=1 -lpthread -lnuma
gcc -O2 -g -c actuator.c -DINIT_DATAPLACEMENT=4 -DMETRIC=2 -lpthread -lnuma
gcc -O2 -o start_threads start_threads.c recorder.o monitor.o intercept_mmap.o actuator.o track_mapping.o  -lrt -lm -lpfm -lpthread -lperf -lnuma;
gcc -O2 -fno-pie preload.c -rdynamic -fpic -shared -o preload.so -ldl -lrt -lnuma;

gcc -fno-pie libsyscall_intercept.c -rdynamic -fpic -shared -o libsyscall_intercept.so -lpthread -lsyscall_intercept

for ((j = 0; j < ${#METRICS[@]}; j++)); do
    echo -n ${METRICS[$j]}
    mkdir -p results/${METRICS[$j]}
    for i in {1..1}
    do
        #echo "Removing files on folder"
        
        echo -n " ."
        rm -f *.txt
        sudo ./delete_shared_memory
        
        #LD_PRELOAD=$(pwd)/libsyscall_intercept.so /scratch/gapbs/./bc -f /scratch/gapbs/benchmark/graphs/urand_u27_k16.sg -n2 1> /dev/null &
        /scratch/gapbs/./bc -f /scratch/gapbs/benchmark/graphs/urand_u27_k16.sg -n2 1> /dev/null &
        #LD_PRELOAD=$(pwd)/preload.so /scratch/gapbs/./bc -f /scratch/gapbs/benchmark/graphs/kron_u29_k16.sg -n2 1> /dev/null &
        #LD_PRELOAD=$(pwd)/preload.so /scratch/gapbs/./bc -f /scratch/gapbs/benchmark/graphs/kron_g30_k16.sg -n2 1> /dev/null &
        
        #autonuma
        #/scratch/gapbs/./bc -f /scratch/gapbs/benchmark/graphs/kron_g29_k16.sg -n2 1> /dev/null &
        #/scratch/gapbs/./bc -f /scratch/gapbs/benchmark/graphs/kron_g30_k16.sg -n2 1> /dev/null &
        app_pid=$!
        
        sudo perf trace -p $app_pid -T --call-graph dwarf -e syscalls:sys_enter_mmap,syscalls:sys_exit_mmap 2> perf_trace_call_stack.txt &
        sudo strace -f -ttt -k -e trace=mmap -p $app_pid 2> strace_call_stack.txt &
        ./track_memory_ranges.sh $app_pid ${METRICS[$j]} &
        
        sudo env MAXIMUM_DRAM_CAPACITY=1000 MINIMUM_SPACE_TO_ACTIVE_DOWNGRADE=4 track_mapping_INTERVAL=0.5 ACTUATOR_INTERVAL=1 MONITOR_INTERVAL=1  ./start_threads $app_pid > "scheduler_output.txt" 2>&1 &
        #sudo env MAXIMUM_DRAM_CAPACITY=33 MINIMUM_SPACE_TO_ACTIVE_DOWNGRADE=4 track_mapping_INTERVAL=0.5 ACTUATOR_INTERVAL=1 MONITOR_INTERVAL=1  ./start_threads $app_pid > "start_thread_output.txt" &
        app_threads=$!

        wait $app_pid
        sudo kill -10 start_threads
        wait $app_threads
        
        
        #Organize output files and clean
        #------------------------------------------------------------------------
        mkdir -p results/${METRICS[$j]}/$app_pid
        
        count=`ls -1 bind_error_* 2>/dev/null | wc -l`
        if [ $count != 0 ]
        then
            cp bind_error_* results/${METRICS[$j]}/$app_pid
        fi

        grep sys_enter_mmap\( perf_trace_call_stack.txt > post_process_perf_trace.txt
        grep recorder_insert scheduler_output.txt > post_process_scheduler.txt
        grep mmap\( strace_call_stack.txt > post_process_strace.txt
        cp *.txt results/${METRICS[$j]}/$app_pid 2> /dev/null
        
        count=`ls -1 preload_migration_cost.txt 2>/dev/null | wc -l`
        if [ $count != 0 ]
        then
            cat preload_migration_cost.txt | awk -F, 'NR!=1{print $6}' | tr -d '(ms)' | datamash min 1 max 1 sum 1 > min_max_sum_migration_cost.txt
            wc -l preload_migration_cost.txt >> min_max_sum_migration_cost.txt
            cp preload_migration_cost.txt results/${METRICS[$j]}/$app_pid
            cp min_max_sum_migration_cost.txt results/${METRICS[$j]}/$app_pid
        fi
        
        rm -f *.txt bind_error_* -f migration.* migration_error.* min_max* *.o
        sleep 5
    done
    echo ""
done


