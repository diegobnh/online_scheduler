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
sudo sh -c 'echo false > /sys/kernel/mm/numa/demotion_enabled'

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
gcc -O1 -I/include -g -c monitor.c -lrt -lm -lpfm -lpthread -lperf;
gcc -O2 -g -c track_mapping.c -DMETRIC=1 -lpthread -lnuma
gcc -O2 -g -c actuator.c -DINIT_DATAPLACEMENT=5 -DMETRIC=2 -lpthread -lnuma
gcc -O2 -o start_threads start_threads.c recorder.o monitor.o intercept_mmap.o actuator.o track_mapping.o  -lrt -lm -lpfm -lpthread -lperf -lnuma;
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

    echo "timestamp,dram_app,pmem_app"  >> $track_info
    while true
    do
    if ps -p $app_pid > /dev/null
    then
        sec=$(date +%s)
        nanosec=$(date +%s)
        timestamp=$(awk '{print $1}' /proc/uptime)
        memory=$(numastat -p $app_pid -c | grep Private | awk '{printf "%s,%s\n", $2,$4}')
        echo $timestamp","$memory >> $track_info
    else
        break
    fi
        sleep 1
    done
}

for ((j = 0; j < ${#METRICS[@]}; j++)); do
    #echo -n ${METRICS[$j]}
    mkdir -p results/${METRICS[$j]}
    for i in {1..1}
    do
        sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches'
        sleep 3

        track_info "bc" "bc_kron" &
        strace -e trace=mmap -- /scratch/gapbs/./bc -f /ihome/dmosse/dmoura/datasets/kron_g26_k16.sg -n1 1> out1.txt 2> out2.txt  &
        app_pid=$!
        wait $app_pid

        echo -n "Maximum Consumption:"
        cat track_info_bc_kron.csv | awk -F, '{print $2+$3}' | datamash max 1 
        echo -n "Maximum in DRAM:"
        cat track_info_bc_kron.csv | awk -F, 'NR>1{print $2}' | datamash max 1
        echo -n "Maximum in PMEM:"
        cat track_info_bc_kron.csv | awk -F, 'NR>1{print $3}' | datamash max 1
        echo "---------------------"
        echo "Different allocations:"
        cat out2.txt | awk -F, '{printf "%.6f\n", $2/1000000000}'  | datamash -sC -g 1 count 1
    done
done


