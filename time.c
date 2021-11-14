#include <time.h>
#include <inttypes.h>


float get_timestamp_diff_in_seconds(struct timespec start, struct timespec end){
    long seconds = end.tv_sec - start.tv_sec;
    long ns = end.tv_nsec - start.tv_nsec;
    if (start.tv_nsec > end.tv_nsec) { // clock underflow
        --seconds;
        ns += 1000000000;
    }
    
    uint64_t delta_us = (end.tv_sec - start.tv_sec) * 1000000 + (end.tv_nsec - start.tv_nsec) / 1000;
    return (float)delta_us/1000000;
}
