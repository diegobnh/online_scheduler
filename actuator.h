#ifndef __ACTUATOR_H_
#define __ACTUATOR_H_

#define METRIC_ABS_LLCM 1
#define METRIC_LLCM_PER_SIZE 2
#define METRIC_ABS_TLB_MISS 3
#define METRIC_TLB_MISS_PER_SIZE 4
#define METRIC_ABS_WRITE 5
#define METRIC_WRITE_PER_SIZE 6
#define METRIC_ABS_LATENCY 7
#define METRIC_LATENCY_PER_SIZE 8

void *thread_actuator(void *);

#endif


