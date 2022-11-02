#!/bin/bash
: '
chmod +777 online_scheduler
sudo ./run.sh our_schedule
'

if [ $# -lt 1 ] ; then
    echo "You must passed three arguments!"
    echo "e.g.  sudo ./run.sh autonuma"
    echo "e.g.  sudo ./run.sh our_schedule"
    exit
fi

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
        sed -i '$ d' $track_info   #Remove the last line. Usally some column is empty
        break
    fi
        sleep 0.20
    done
}

METRICS=("LLCM_PER_SIZE")
#METRICS=("ABS_LLCM" "LLCM_PER_SIZE" "ABS_TLB_MISS" "TLB_MISS_PER_SIZE" "ABS_WRITE" "WRITE_PER_SIZE" "ABS_LATENCY" "LATENCY_PER_SIZE")

mkdir -p results
rm -f migration_*.pipe

#Initial Dataplacement Options
#1 - ROUND_ROBIN
#2 - RANDOM
#3 - FIRST_DRAM
#4 - FIRST_PMEM
#5 - BASED ON SIZE

#bind this script to run in only one socket . In our case, node 0!
export OMP_NUM_THREADS=18
export OMP_PLACES={0}:18:2
export OMP_PROC_BIND=true

gcc -o delete_shared_memory delete_shared_memory.c -lrt
gcc -O2 -g -c recorder.c -lpthread;
gcc -O2 -I/include -g -c intercept_mmap.c -lpthread;
gcc -O2 -I/include -g -c monitor.c hashmap.c -lrt -lm -lpfm -lpthread -lperf;
gcc -O2 -g -c track_mapping.c -DMETRIC=1 -lpthread -lnuma
gcc -O2 -g -c actuator.c -DINIT_DATAPLACEMENT=4 -DMETRIC=2 -lpthread -lnuma
gcc -O2 -o start_threads start_threads.c recorder.o monitor.o hashmap.o intercept_mmap.o actuator.o track_mapping.o  -lrt -lm -lpfm -lpthread -lperf -lnuma;
gcc -O2 -fno-pie preload.c -rdynamic -fpic -shared -o preload.so -ldl -lrt -lnuma;
gcc -fno-pie libsyscall_intercept.c -rdynamic -fpic -shared -o libsyscall_intercept.so -lpthread -lsyscall_intercept



if [[ $1 == "autonuma" ]]; then
    setup_autonuma_parameters
    track_info "bc" "bc_kron" &
	/mnt/myPMEM/gapbs/./bc -f /mnt/myPMEM/gapbs/benchmark/graphs/kron.sg -n3 1> /dev/null &
    app_pid=$!
    wait $app_pid
    mkdir -p results/autonuma/$app_pid
    python3 plots/plot_mem_usage.py
    mv mem_usage*.pdf results/autonuma/$app_pid
    mv mem_consumption.txt results/autonuma/$app_pid
    mv track_info* exec_time.txt results/autonuma/$app_pid
elif [[ $1 == "our_schedule" ]] ; then
    setup_our_schedule_mapping_parameters
    mkdir -p results/our_schedule/
    for i in {1..1}
    do
        sleep 3
        sudo rm -f *.txt bind_error_* -f min_max* *.o
        sudo ./delete_shared_memory

        track_info "bc" "bc_kron" &
        sudo env TRACK_MAPPING_INTERVAL=0.5 ACTUATOR_INTERVAL=1 MONITOR_INTERVAL=1  ./start_threads > "scheduler_output.txt"  2>&1 &
        start_threads_pid=$!

        sleep 3  #if you dont wait, you could lose some mmaps'interception
        echo "starting the preload"
        LD_PRELOAD=$(pwd)/preload.so /mnt/myPMEM/gapbs/./bc -f /mnt/myPMEM/gapbs/benchmark/graphs/kron.sg -n3 1> /dev/null &

        app_pid=$!
        #echo $app_pid > pid.txt

        wait $app_pid
        sudo kill -10 start_threads
        wait $start_threads_pid

        mkdir -p results/our_schedule/$app_pid
        python3 plots/plot_mem_usage.py
        mv mem_usage*.pdf results/our_schedule/$app_pid
        mv track_info* results/our_schedule/$app_pid
    done
else
    echo "Invalid parameter!"
fi;
