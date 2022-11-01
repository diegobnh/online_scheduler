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

if [ $# -lt 1 ] ; then
    echo "You must passed three arguments!"
    echo "e.g.  sudo ./run.sh autonuma"
    echo "e.g.  sudo ./run.sh our_schedule"
    exit
fi

#bind this script to run in only one socket . In our case, node 0!
export OMP_NUM_THREADS=18
export OMP_PLACES={0}:18:2
export OMP_PROC_BIND=true



function setup_our_schedule_mapping_parameters {
    sudo sysctl -w kernel.perf_event_max_sample_rate=10000 1> /dev/null
    sudo sysctl -w kernel.numa_balancing=0 >  /dev/null
    sudo sh -c "echo false > /sys/kernel/mm/numa/demotion_enabled"
    sudo sh -c "echo 65536 > /proc/sys/kernel/numa_balancing_rate_limit_mbps"
    sudo sh -c "echo 0 > /proc/sys/kernel/numa_balancing_wake_up_kswapd_early"
    sudo sh -c "echo 0 > /proc/sys/kernel/numa_balancing_scan_demoted"
    sudo sh -c "echo 0 > /proc/sys/kernel/numa_balancing_demoted_threshold"
    #sudo sh -c "echo false > /sys/kernel/mm/numa/demotion_enabled"
    sudo sysctl -w vm.vfs_cache_pressure=100 > /dev/null
    sudo sysctl -w vm.drop_caches=3 > /dev/null
}

function setup_autonuma_parameters {
    sudo sysctl -w kernel.perf_event_max_sample_rate=10000 1> /dev/null
    sudo sysctl -w kernel.numa_balancing=2 >  /dev/null
    sudo sh -c "echo true > /sys/kernel/mm/numa/demotion_enabled"
    sudo sh -c "echo 65536 > /proc/sys/kernel/numa_balancing_rate_limit_mbps"
    sudo sh -c "echo 0 > /proc/sys/kernel/numa_balancing_wake_up_kswapd_early"
    sudo sh -c "echo 0 > /proc/sys/kernel/numa_balancing_scan_demoted"
    sudo sh -c "echo 0 > /proc/sys/kernel/numa_balancing_demoted_threshold"
    #sudo sh -c "echo true > /sys/kernel/mm/numa/demotion_enabled"
    sudo sysctl -w vm.vfs_cache_pressure=100 > /dev/null
    sudo sysctl -w vm.drop_caches=3 > /dev/null
}


#Disable address space layout randomization the
#echo 0 | sudo tee /proc/sys/kernel/randomize_va_space

METRICS=("LLCM_PER_SIZE")
#METRICS=("ABS_LLCM" "LLCM_PER_SIZE" "ABS_TLB_MISS" "TLB_MISS_PER_SIZE" "ABS_WRITE" "WRITE_PER_SIZE" "ABS_LATENCY" "LATENCY_PER_SIZE")

mkdir -p results
#rm -rf results/*

#Initial Dataplacement Options
#1 - ROUND_ROBIN
#2 - RANDOM
#3 - FIRST_DRAM
#4 - FIRST_PMEM
#5 - BASED ON SIZE

gcc -o delete_shared_memory delete_shared_memory.c -lrt
gcc -O2 -g -c recorder.c -lpthread;
gcc -O2 -I/include -g -c intercept_mmap.c -lpthread;
gcc -O2 -I/include -g -c monitor.c hashmap.c -lrt -lm -lpfm -lpthread -lperf;
gcc -O2 -g -c track_mapping.c -DMETRIC=1 -lpthread -lnuma
gcc -O2 -g -c actuator.c -DINIT_DATAPLACEMENT=4 -DMETRIC=2 -lpthread -lnuma
gcc -O2 -o start_threads start_threads.c recorder.o monitor.o hashmap.o intercept_mmap.o actuator.o track_mapping.o  -lrt -lm -lpfm -lpthread -lperf -lnuma;
gcc -O2 -fno-pie preload.c -rdynamic -fpic -shared -o preload.so -ldl -lrt -lnuma;
gcc -fno-pie libsyscall_intercept.c -rdynamic -fpic -shared -o libsyscall_intercept.so -lpthread -lsyscall_intercept

function track_info {
    rm -rf track_info*
    #While application dosen't exist, we continue in this loop
    while true
    do
        app_pid=$(pidof $1)
        if ps -p $app_pid > /dev/null 2> /dev/null
        then
            break
        fi
    done

    track_info="track_info_"$2".csv"
    rm -f $track_info

    echo "timestamp,dram_app,pmem_app,dram_page_cache_active,dram_page_cache_inactive"  >> $track_info
    while true
    do
    if ps -p $app_pid > /dev/null
    then
        sec=$(date +%s)
        nanosec=$(date +%s)
        timestamp=$(awk '{print $1}' /proc/uptime)
        memory=$(numastat -p $app_pid -c | grep Private | awk '{printf "%s,%s\n", $2,$4}')
        dram_page_cache=$(grep "Active(file)\|Inactive(file)" /sys/devices/system/node/node0/meminfo | awk '{print $(NF-1)}' | datamash transpose | awk '{printf "%s,%s\n", $1, $2}')
        
        echo $timestamp","$memory","$dram_page_cache >> $track_info
    else
        sed -i '$ d' $track_info   #Remove the last line. Usally some column is empty
        break
    fi
        sleep 1
    done
}


if [[ $1 == "autonuma" ]]; then
    setup_autonuma_parameters
    track_info "bc" "bc_kron" &
	/scratch/gapbs/./bc -f /ihome/dmosse/dmoura/datasets/kron_g27_k16.sg -n1 1> /dev/null
    
elif [[ $1 == "our_schedule" ]] ; then
    setup_our_schedule_mapping_parameters
    
    for ((j = 0; j < ${#METRICS[@]}; j++)); do
        #echo -n ${METRICS[$j]}
        mkdir -p results/${METRICS[$j]}
        for i in {1..1}
        do
            sleep 3
            sudo rm -f *.txt bind_error_* -f min_max* *.o
            sudo ./delete_shared_memory

            track_info "bc" "bc_kron" &
            sudo env TRACK_MAPPING_INTERVAL=0.5 ACTUATOR_INTERVAL=1 MONITOR_INTERVAL=1  ./start_threads > "scheduler_output.txt"  2>&1 &
            start_threads_pid=$!

            sleep 3  #if you dont wait, you could lose some mmaps'interception

            LD_PRELOAD=$(pwd)/preload.so /scratch/gapbs/./bc -f /ihome/dmosse/dmoura/datasets/kron_g27_k16.sg -n1 1> /dev/null &

            app_pid=$!
            echo $app_pid > pid.txt

            #./track_memory_ranges.sh $app_pid ${METRICS[$j]} &

            wait $app_pid
            sudo kill -10 start_threads
            wait $start_threads_pid


            #Organize output files and clean
            #------------------------------------------------------------------------
            sleep 3
            mkdir -p results/${METRICS[$j]}/$app_pid
            rm -f mem_consumption.txt


            echo -n "DRAM(max) memfootprint    : " >> mem_consumption.txt
            cat track_info_bc_kron.csv | awk -F, 'NR>1{print $2}' | datamash max 1 >> mem_consumption.txt

            echo -n "(DRAM + PMEM) memfootprint: " >> mem_consumption.txt
            cat track_info_bc_kron.csv | awk -F, '{print $2+$3}' | datamash max 1 >> mem_consumption.txt

            echo -n "Page Cache DRAM (max)     : " >> mem_consumption.txt
            cat track_info_bc_kron.csv | awk -F, '{print ($4+$5)/1000}' | datamash max 1 >> mem_consumption.txt

            #cp bind_error_* results/${METRICS[$j]}/$app_pid 2>/dev/null
            cat preload_migration_cost.txt | awk -F, 'NR!=1{print $6}' | tr -d '(ms)' | datamash min 1 max 1 sum 1 > min_max_sum_migration_cost.txt
            wc -l preload_migration_cost.txt >> min_max_sum_migration_cost.txt
            mv *.txt *.csv results/${METRICS[$j]}/$app_pid/
            cp bind_error_* results/${METRICS[$j]}/$app_pid 2>/dev/null
            rm *.pipe
            sleep 10
        done
    done
else
    echo "Invalid parameter!"
fi;




