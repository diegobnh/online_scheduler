#!/bin/bash

export OMP_NUM_THREADS=18
export OMP_PLACES={0}:18:2 #bind this script to run in only one socket . In our case, node 0!
export OMP_PROC_BIND=true

sudo sysctl -w kernel.perf_event_max_sample_rate=10000 1> /dev/null
sudo sysctl -w kernel.numa_balancing=2 >  /dev/null
sudo sh -c "echo true > /sys/kernel/mm/numa/demotion_enabled"
sudo sh -c "echo 65536 > /proc/sys/kernel/numa_balancing_rate_limit_mbps"
sudo sh -c "echo 0 > /proc/sys/kernel/numa_balancing_wake_up_kswapd_early"
sudo sh -c "echo 0 > /proc/sys/kernel/numa_balancing_scan_demoted"
sudo sh -c "echo 0 > /proc/sys/kernel/numa_balancing_demoted_threshold"
sudo sysctl -w vm.vfs_cache_pressure=100 > /dev/null


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

    track_info="track_info_"$2"_autonuma.csv"
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
    
    start=$(sed -n 2p "track_info_"$2"_autonuma.csv" | awk -F, '{print $1}')
    end=$(tail -n 1 "track_info_"$2"_autonuma.csv" | awk -F, '{print $1}')
    exec_time_autonuma=$(echo $start $end | awk '{print ($2-$1)}') #seconds
    now=$(date +"%T")
    echo $exec_time_autonuma > "autonuma_"$2"_"$now".txt"
	    
}

rm -f autonuma_*.txt

for i in 1 2 3
do
	echo -n $i
	echo -n " "

	sudo sysctl -w vm.drop_caches=3 > /dev/null
	sleep 10

	track_info "bc" "bc_kron" &
	/scratch/gapbs/./bc -f /ihome/dmosse/dmoura/datasets/kron_g27_k16.sg -n1 1> /dev/null

	sleep 10

done


