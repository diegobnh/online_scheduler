#!/bin/bash
: '
git clone https://github.com/diegobnh/online_scheduler.git; chmod +777 online_scheduler; cd online_scheduler
sudo ./run.sh our_schedule 2> /dev/null
sudo ./run.sh autonuma 2> /dev/null
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

function post_process_perfmem {
    perf script -f --comms=bc | sed 's/cpu\/mem-loads,ldlat=30\/P:/loads/g' | sed 's/cpu\/mem-stores\/P:/stores/g' | grep -w "loads" | sed 's/Local RAM or RAM/DRAM_hit/g' | sed 's/LFB or LFB hit/LFB_hit/g' | sed 's/L1 or L1 hit/L1_hit/g' | sed 's/L2 or L2 hit/L2_hit/g' | sed 's/L3 or L3 hit/L3_hit/g' | sed 's/L3 miss/L3_miss/g' | sed 's/PMEM hit/NVM_hit/g' | tr -d ":" | sed 's/|SNP//g' | awk '{OFS=","}{print $4,"0x"$7,$9}' | grep "NVM_hit\|DRAM_hit" > loads.txt 
}

METRICS=("LLCM_PER_SIZE")
#METRICS=("ABS_LLCM" "LLCM_PER_SIZE" "ABS_TLB_MISS" "TLB_MISS_PER_SIZE" "ABS_WRITE" "WRITE_PER_SIZE" "ABS_LATENCY" "LATENCY_PER_SIZE")

mkdir -p results

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

gcc -o lock_memory lock_memory.c
gcc -o delete_shared_memory delete_shared_memory.c -lrt
gcc -O2 -g -c recorder.c -lpthread;
gcc -O2 -I/include -g -c intercept_mmap.c -lpthread;
gcc -O2 -I/include -g -c monitor.c hashmap.c -lrt -lm -lpfm -lpthread -lperf;
gcc -O2 -g -c track_mapping.c -DMETRIC=1 -lpthread -lnuma
gcc -O2 -g -c actuator.c -DINIT_DATAPLACEMENT=4 -DMETRIC=2 -lpthread -lnuma
gcc -O2 -o start_threads start_threads.c recorder.o monitor.o hashmap.o intercept_mmap.o actuator.o track_mapping.o  -lrt -lm -lpfm -lpthread -lperf -lnuma;
gcc -O2 -fno-pie preload.c -rdynamic -fpic -shared -o preload.so -ldl -lrt -lnuma;
gcc -fno-pie libsyscall_intercept.c -rdynamic -fpic -shared -o libsyscall_intercept.so -lpthread -lsyscall_intercept

#bc kron has around 20GB of footprint. We have 17.5 dram space. So, reduce 8 GB we will force around 50% of access in NVM.
numactl --membind=0 ./lock_memory 8 &
lock_memory_pid=$!
sleep 5

perf mem -D --phys-data record -k CLOCK_MONOTONIC --all-user 2> /dev/null &

if [[ $1 == "autonuma" ]]; then
    setup_autonuma_parameters
    track_info "bc" "bc_kron" &
	/mnt/myPMEM/gapbs/./bc -f /mnt/myPMEM/gapbs/benchmark/graphs/kron.sg -n3 1> /dev/null &
    app_pid=$!
    wait $app_pid
    
    pkill perf &> /dev/null
    sleep 5
    post_process_perfmem
	
    mkdir -p results/autonuma/$app_pid
    mv mem_usage*.pdf results/autonuma/$app_pid
    mv loads.txt track_info* results/autonuma/$app_pid
    cd results/autonuma/$app_pid
    python3 ../../../plots/plot_mem_usage.py autonuma
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
        LD_PRELOAD=$(pwd)/preload.so /mnt/myPMEM/gapbs/./bc -f /mnt/myPMEM/gapbs/benchmark/graphs/kron.sg -n3 1> /dev/null &

        app_pid=$!
        echo $app_pid > pid.txt

        wait $app_pid
        sudo kill -10 start_threads
        wait $start_threads_pid

	pkill perf &> /dev/null
	sleep 5
	post_process_perfmem

        mkdir -p results/our_schedule/$app_pid
        mv mem_usage*.pdf results/our_schedule/$app_pid
        mv loads.txt track_info* scheduler_output.txt results/our_schedule/$app_pid
	results/our_schedule/$app_pid
	python3 ../../../plots/plot_mem_usage.py our_schedule
    done
else
    echo "Invalid parameter!"
fi;


kill -10 $lock_memory_pid
rm -f migration_*.pipe pid.txt
