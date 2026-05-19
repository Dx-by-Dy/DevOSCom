#ifndef __WRITE_H
#define __WRITE_H

#include <bpf/bpf_endian.h>

#define TASK_COMM_LEN 16

struct start_data {
    __u64 ts;
};

struct event {
    __u32 tgid;
    __u32 tid;

    char comm[TASK_COMM_LEN];

    __u64 latency_ns;
    __s64 ret;
};

#endif