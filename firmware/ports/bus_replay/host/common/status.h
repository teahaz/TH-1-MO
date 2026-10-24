/*
 * Counters and status of a capture, shared by the live backends, capture files and the UI.
 */

#ifndef COMMON_STATUS_H
#define COMMON_STATUS_H

#include <stdbool.h>
#include <stdint.h>


/*
 * One data channel's stream, as parsed. A mismatch is a heartbeat whose levels disagree
 * with the records before it; one inside a reported loss is excused instead.
 */
struct data_stats {
    uint64_t bytes;
    uint64_t records;
    uint64_t heartbeats;
    uint64_t crc_errors;
    uint64_t gap_errors;
    uint64_t resyncs;
    uint64_t mismatches;
    uint64_t excused;
};

// A capture as a whole: whether it runs, how far its streams have got, its loss reports.
struct capture_status {
    bool running;
    uint64_t now;
    uint64_t losses;
    struct data_stats total;
};

#endif
