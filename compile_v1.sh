#!/bin/bash

#chmod +x compile.sh; time numactl --cpubind=0 --membind=2 ./compile.sh

#chmod +x compile.sh;for i in {1..2}; do ./compile.sh > "output_"$i".txt" 2>&1; done
#chmod +x compile.sh;for i in {1..3}; do sleep 3; sed -i 's/DMETRIC=3/DMETRIC=4/' compile.sh; numactl --cpubind=0 --membind=2 ./compile.sh > "output_metric1_"$i".txt" 2>&1; done

#grep "Execution_time*" results/ABS_LLCM/*/output*

export OMP_NUM_THREADS=18
export OMP_PLACES={0}:18:2
export OMP_PROC_BIND=true

mkdir -p results
rm -rf results/*

rm -f migration_cost.*
rm -f bind_error_*
sudo rm -f /tmp/migration.*
sudo rm -f /tmp/migration_error.*

gcc -o delete_shared_memory delete_shared_memory.c -lrt
gcc -O2 -g -c recorder.c -lpthread;
gcc -O2 -I/include -g -c intercept_mmap.c -lpthread;
gcc -O0 -I/include -g -c monitor.c -lrt -lm -lpfm -lpthread -lperf;
gcc -O2 -g -c actuator.c -DINIT_DATAPLACEMENT=4 -DMETRIC=2 -lpthread -lnuma
gcc -O2 -fno-pie preload.c -rdynamic -fpic -shared -o preload.so -ldl -lrt -lnuma;
gcc -O2 -o start_threads start_threads.c recorder.o actuator.o monitor.o intercept_mmap.o  -lrt -lm -lpfm -lpthread -lperf -lnuma;

./delete_shared_memory

LD_PRELOAD=$(pwd)/preload.so /scratch/gapbs/./bc -f /scratch/gapbs/benchmark/graphs/kron.sg -n2 1> /dev/null &
app_pid=$!
        
sudo env MAXIMUM_DRAM_CAPACITY=4 MINIMUM_SPACE_TO_ACTIVE_DOWNGRADE=0.2 ACTUATOR_INTERVAL=2 MONITOR_INTERVAL=1  ./start_threads $app_pid &
app_threads=$!

wait $app_pid
sudo pkill -27 start_threads
wait $app_threads



