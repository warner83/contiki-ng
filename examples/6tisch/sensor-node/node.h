#ifndef __SENSOR_NODE__
#define __SENSOR_NODE__

#include "random.h"

#define MAX_PKTS 10
#define DEFAULT_PAYLOAD_LEN 50
#define MAX_RUNS 2
#define MAX_SOURCE_NODES 16
#define NSEQNOS 4

/* random interval uniformly distributed btw. min and max values,
 * in terms of etimer clock ticks
 * to set a fixed interval, simply set min and max to the same value
 */
#define SEND_INTERVAL_MIN (CLOCK_SECOND * 1 / 1)
#define SEND_INTERVAL_MAX (CLOCK_SECOND * 1 / 1)
#define SEND_INTERVAL_DELTA (SEND_INTERVAL_MAX - SEND_INTERVAL_MIN)
#define SEND_INTERVAL_AVG ((SEND_INTERVAL_MAX + SEND_INTERVAL_MIN) / 2)
#define SEND_INTERVAL (SEND_INTERVAL_MIN + random_rand() % (SEND_INTERVAL_DELTA + 1))

#endif /* __SENSOR_NODE__ */
