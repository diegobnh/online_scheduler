#!/bin/bash

app_pid=$1
metric=$2
out1="numa_maps.txt"
out2="maps.txt"
rm -f $out1
rm -f $out2

while true
do
	if ps -p $app_pid > /dev/null
	then
	    sec=$(date +%s)
	    nanosec=$(date +%s)
	    echo $sec"."$nanosec >> $out1
   		cat /proc/$app_pid/numa_maps  >> $out1 2> /dev/null
   		#cat /proc/$app_pid/numa_maps | grep bind  >> $out1
   		echo " " >>  $out1
   		
   		echo $sec"."$nanosec >> $out2
   		cat /proc/$app_pid/maps >> $out2
   		echo " " >>  $out2
	else
   		break
	fi
        sleep 5
done 

mv $out1 $out2 results/$metric/$app_pid 2> /dev/null
