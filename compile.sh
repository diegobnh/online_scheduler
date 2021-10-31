#!/bin/bash
unset APP
export APP="gapbs"

#OMP_NUM_THREADS=1
#export OMP_NUM_THREADS

gcc -o delete_shared_memory delete_shared_memory.c -lrt
gcc -g -c -fPIC time.c
gcc -g -c -fPIC recorder.c -lpthread;
gcc -g -c -fPIC actuator.c -lrt -lnuma;
gcc -g -c -fPIC sample_processor.c

#gcc -g -c monitor_binary_search.c -lrt -lm -lnuma
#gcc -o monitor monitor.c actuator.o recorder.o monitor_binary_search.o -lrt -lm -lpfm -lnuma
gcc -o monitor monitor.c actuator.o recorder.o sample_processor.o time.o -lrt -lm -lpfm -lnuma -lpthread

gcc -g -o main main.c
#gcc -g malloc_intercept.c -fpic -shared -o malloc_intercept.so recorder.o -rdynamic -ldl -lpthread -lrt
gcc -fno-pie mmap_intercept.c -rdynamic -fpic -shared -o mmap_intercept.so recorder.o actuator.o -lpthread -lrt -lnuma -lsyscall_intercept;

#In case the application in the last execution has finished without remove shared memory
./delete_shared_memory

#Start the main that will spawn other applications
#LD_PRELOAD=./mmap_intercept.so ./main 1> /dev/null &
LD_PRELOAD=./mmap_intercept.so /scratch/gapbs/./bc -f /scratch/gapbs/benchmark/graphs/kron.sg -n2 1> /dev/null &

#./main 1> /dev/null &
pid_main=$!
#taskset -cp 0 $pid_main
#LD_PRELOAD=./malloc_intercept.so ./main 1> /dev/null


#Start independent monitor
#sleep 1
#./monitor &
#pid_monitor=$!

#When the main finish, send a signal to monitor finish
wait $pid_main
#kill -27 $pid_monitor
 
