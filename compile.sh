#!/bin/bash

#./compile.sh 2> out2 

OMP_NUM_THREADS=18
export OMP_NUM_THREADS

rm -f *.trace
gcc -o delete_shared_memory delete_shared_memory.c -lrt
gcc -g -c -fPIC time.c
gcc -g -c -fPIC recorder.c -lpthread;
gcc -g -c -fPIC actuator.c -lrt -lnuma;
#gcc -g -c -fPIC sample_processor_v3.c
#gcc -g -c -fPIC sample_processor.c
gcc -g -I/scratch/build/autonuma-r6/tools/lib/perf/include/ -o monitor monitor_vinicius.c recorder.o -lrt -lm -lpfm -lpthread -lperf -lnuma
gcc -g -fno-pie mmap_intercept.c -rdynamic -fpic -shared -o mmap_intercept.so recorder.o actuator.o -lpthread -lrt -lnuma -lsyscall_intercept -DINIT_ALLOC=4;

#In case the application in the last execution has finished without remove shared memory
./delete_shared_memory

#sudo echo 1 > /proc/sys/vm/drop_caches
sudo sh -c "/usr/bin/echo 1 > /proc/sys/vm/drop_caches"

SECONDS=0
LD_PRELOAD=./mmap_intercept.so /scratch/gapbs/./bc -f /scratch/gapbs/benchmark/graphs/kron.sg -n1 1> /dev/null &
pid_app=$!

taskset -cp 0,2,4,6,8,10,12,14,16,18,20,22,24,26,28,30,32,34  $pid_app 1> /dev/null

sleep 1
sudo ./monitor &
pid_monitor=$!


#When the main finish, send a signal to monitor finish
wait $pid_app
kill -27 $pid_monitor

ELAPSED="Elapsed: $((($SECONDS / 60) % 60))min $(($SECONDS % 60))sec"
echo $ELAPSED >&2
