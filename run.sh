#!/bin/bash
: '
sudo ./run.sh our_schedule 2> /dev/null
sudo ./run.sh autonuma 2> /dev/null
'

MONITOR_INTERVAL=(0.5 1 2)
ACTUATOR_INTERVAL=(0.5 1 2)
HOTNESS_THRESHOLD=(0 50 100)
SAMPLE_FREQ=(50 100 200)

TOTAL_ITERATIONS=10

export OMP_NUM_THREADS=18
export OMP_PLACES={0}:18:2
export OMP_PROC_BIND=true

gcc -o lock_memory lock_memory.c
gcc -o delete_shared_memory delete_shared_memory.c -lrt
gcc -O2 -g -c recorder.c -lpthread;
gcc -O2 -I/include -g -c intercept_mmap.c -lpthread;
gcc -O2 -I/include -g -c monitor.c hashmap.c -lrt -lm -lpfm -lpthread -lperf;
#gcc -O2 -g -c track_mapping.c -DMETRIC=1 -lpthread -lnuma
gcc -O2 -g -c actuator.c -DINIT_DATAPLACEMENT=4 -DMETRIC=2 -lpthread -lnuma
gcc -O2 -o start_threads start_threads.c recorder.o monitor.o hashmap.o intercept_mmap.o actuator.o  -lrt -lm -lpfm -lpthread -lperf -lnuma;
gcc -O2 -fno-pie preload.c -rdynamic -fpic -shared -o preload.so -ldl -lrt -lnuma;
gcc -fno-pie libsyscall_intercept.c -rdynamic -fpic -shared -o libsyscall_intercept.so -lpthread -lsyscall_intercept



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

    echo "timestamp,dram_app,nvm_app"  >> $track_info
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

function clean_and_start {
    sudo pkill -9 lock_memory
    sudo pkill -9 perf
    sudo pkill -9 start_threads
    sudo pkill -9 bc

    numactl --membind=0 ../../.././lock_memory 8 &
    lock_memory_pid=$!
    sleep 5
    perf mem -D --phys-data record -k CLOCK_MONOTONIC --all-user 2> /dev/null &
}


SECONDS=0

if [[ $1 == "autonuma" ]]; then
    mkdir -p results/autonuma/
    cd results/autonuma/
    for ((t = 0; t < $TOTAL_ITERATIONS; t++)); do		
	    clean_and_start
        setup_autonuma_parameters
        track_info "bc" "bc_kron" &
	    /mnt/myPMEM/gapbs/./bc -f /mnt/myPMEM/gapbs/benchmark/graphs/kron.sg -n3 1> /dev/null &
        app_pid=$!
        wait $app_pid
    
        pkill perf &> /dev/null
        sleep 30
        post_process_perfmem
        mkdir -p $t
        mv loads.txt track_info* $t
        cd $t
        python3 ../../../plots/plot_mem_usage.py autonuma    
    done
    
elif [[ $1 == "our_schedule" ]] ; then
    echo "Running: Our schedule"
    echo "---------------------"
    echo " "
    mkdir -p results/our_schedule/
    cd results/our_schedule/ 
    for ((i = 0; i < ${#MONITOR_INTERVAL[@]}; i++)); do
        for ((j = 0; j < ${#ACTUATOR_INTERVAL[@]}; j++)); do
            for ((k = 0; k < ${#HOTNESS_THRESHOLD[@]}; k++)); do
	            for ((w = 0; w < ${#SAMPLE_FREQ[@]}; w++)); do
                        folder="monitor_"${MONITOR_INTERVAL[$i]}"_actuator_"${ACTUATOR_INTERVAL[$j]}"_hotness_"${HOTNESS_THRESHOLD[$k]}"_sampling_"${SAMPLE_FREQ[$w]}
                        mkdir -p $folder
		        cd $folder
                        echo -n "Parameters:"$folder " , Iter: "
                    	for ((t = 0; t < $TOTAL_ITERATIONS; t++)); do		
			    echo -n $t" "
			    clean_and_start
			    setup_our_schedule_mapping_parameters

			    sleep 3
		    	    sudo rm -f *.txt -f min_max* *.o
			    sudo ../../.././delete_shared_memory

			    track_info "bc" "bc_kron" &

			    sudo env MONITOR_INTERVAL=${MONITOR_INTERVAL[$i]} ACTUATOR_INTERVAL=${ACTUATOR_INTERVAL[$j]} HOTNESS_THRESHOLD=${HOTNESS_THRESHOLD[$k]} SAMPLE_FREQ=${SAMPLE_FREQ[$w]}  ../../.././start_threads > "scheduler_output.txt" 2>&1 &
			    start_threads_pid=$!

			    sleep 3  #if you dont wait, you could lose some mmaps interception
			    cp ../../../preload.so .
			    LD_PRELOAD=$(pwd)/preload.so /mnt/myPMEM/gapbs/./bc -f /mnt/myPMEM/gapbs/benchmark/graphs/kron.sg -n3  1> /dev/null &
			    app_pid=$!
			    echo $app_pid > pid.txt

			    wait $app_pid
		 	    sudo kill -10 start_threads
			    wait $start_threads_pid

			    pkill perf &> /dev/null
			    sleep 30
			    post_process_perfmem
			    rm preload.so
			    rm -f migration_*.pipe pid.txt maps_* perf.data*

			    mkdir -p $t
			    sed -i '1 i\timestamp, obj_index, start_addr, size, nodemask_target_node,  status_pages_before, migration_cost_ms, status_pages_after, bind_type' preload_migration_cost.txt
			    mv preload_migration_cost.txt loads.txt track_info* scheduler_output.txt $t
			    cd $t
			    python3 ../../../../plots/plot_mem_usage.py our_schedule
			    python3 ../../../../plots/plot_migration_info.py

			    kill -10 $lock_memory_pid
			    cd ..
                        done
                        echo " "
                    
                        cd ..
                done
            done
        done
    done
else
    echo "Invalid parameter!"
fi;

duration=$SECONDS
echo "$(($duration / 60)) minutes and $(($duration % 60)) seconds elapsed."
