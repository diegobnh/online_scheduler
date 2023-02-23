#!/bin/bash

source app_dataset.sh

: '
cd results/autonuma/
for ((j = 0; j < ${#APP_DATASET[@]}; j++)); do
    cd ${APP_DATASET[$j]}
        echo -n ${APP_DATASET[$j]}"," > statistics.txt
        cat */exec_time.txt | datamash mean 1 min 1 max 1 sstdev 1 --round=2 | sed 's/\s\+/,/g' >> statistics.txt
        #cat */exec_time.txt | datamash mean 1 --round=2 | sed 's/\s\+/,/g' >> statistics.txt
    cd ..
done
cd ../..
'
cd results/our_schedule/

for ((j = 0; j < ${#APP_DATASET[@]}; j++)); do
    cd ${APP_DATASET[$j]}
        for folder in */ ; do 
            cd $folder
            #for iter in */ ; do 
            #     cd $iter
            #     python3 -W ignore ../../../../../plots/plot_mem_usage_v2.py our_schedule
            #     python3 -W ignore ../../../../../plots/plot_migration_info.py
            #     cd ..
            #done

            echo -n ${APP_DATASET[$j]}"," > statistics.txt
            #echo -n $folder"," > statistics.txt
            cat */exec_time.txt | datamash mean 1 min 1 max 1 sstdev 1 --round=2 | sed 's/\s\+/,/g' >> statistics.txt
            #cat */exec_time.txt | datamash mean 1 --round=2 | sed 's/\s\+/,/g' >> statistics.txt
            cd ..
        done
    cd ..
done
