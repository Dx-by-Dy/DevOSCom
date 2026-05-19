#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include <bpf/libbpf.h>

#include "write.skel.h"
#include "write.h"

static volatile int running = 1;

static void sig_handler(int signo)
{
    running = 0;
}

struct pid_stat
{
    __u32 tgid;

    unsigned long long calls;
    unsigned long long total_ns;
    unsigned long long max_ns;
};

struct comm_stat
{
    char comm[TASK_COMM_LEN];

    unsigned long long calls;
    unsigned long long total_ns;
};

#define MAX_STATS 4096

static struct pid_stat pid_stats[MAX_STATS];
static int pid_stats_count = 0;

static struct comm_stat comm_stats[MAX_STATS];
static int comm_stats_count = 0;

static unsigned long long global_calls = 0;
static unsigned long long global_latency = 0;

static struct pid_stat *find_pid(__u32 tgid)
{
    for (int i = 0; i < pid_stats_count; i++)
    {
        if (pid_stats[i].tgid == tgid)
            return &pid_stats[i];
    }

    if (pid_stats_count >= MAX_STATS)
        return NULL;

    struct pid_stat *s = &pid_stats[pid_stats_count++];
    memset(s, 0, sizeof(*s));
    s->tgid = tgid;

    return s;
}

static struct comm_stat *find_comm(const char *comm)
{
    for (int i = 0; i < comm_stats_count; i++)
    {
        if (!strcmp(comm_stats[i].comm, comm))
            return &comm_stats[i];
    }

    if (comm_stats_count >= MAX_STATS)
        return NULL;

    struct comm_stat *s = &comm_stats[comm_stats_count++];
    memset(s, 0, sizeof(*s));
    strncpy(s->comm, comm, TASK_COMM_LEN);

    return s;
}

static int handle_event(void *ctx, void *data, size_t len)
{
    const struct event *e = data;
    struct pid_stat *p = find_pid(e->tgid);
    struct comm_stat *c = find_comm(e->comm);

    if (p)
    {
        p->calls++;
        p->total_ns += e->latency_ns;

        if (e->latency_ns > p->max_ns)
            p->max_ns = e->latency_ns;
    }

    if (c)
    {
        c->calls++;
        c->total_ns += e->latency_ns;
    }

    global_calls++;
    global_latency += e->latency_ns;

    return 0;
}

static void print_stats(void)
{
    printf("\n=== PID stats ===\n");
    for (int i = 0; i < pid_stats_count; i++)
    {

        double avg_us =
            (double)pid_stats[i].total_ns /
            pid_stats[i].calls /
            1000.0;

        printf(
            "tgid=%u calls=%llu avg=%.2f us max=%.2f us\n",
            pid_stats[i].tgid,
            pid_stats[i].calls,
            avg_us,
            pid_stats[i].max_ns / 1000.0);
    }

    printf("\n=== COMM stats ===\n");
    for (int i = 0; i < comm_stats_count; i++)
    {
        double avg_us =
            (double)comm_stats[i].total_ns /
            comm_stats[i].calls /
            1000.0;

        printf(
            "comm=%s calls=%llu avg=%.2f us\n",
            comm_stats[i].comm,
            comm_stats[i].calls,
            avg_us);
    }

    printf("\n=== GLOBAL ===\n");
    if (global_calls)
    {
        double avg_us =
            (double)global_latency /
            global_calls /
            1000.0;

        printf(
            "calls=%llu avg=%.2f us\n",
            global_calls,
            avg_us);
    }
}

int main(void)
{
    struct ring_buffer *rb = NULL;
    struct write_bpf *skel;

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    skel = write_bpf__open_and_load();
    if (!skel)
    {
        fprintf(stderr, "failed to open skeleton\n");
        return 1;
    }

    if (write_bpf__attach(skel))
    {
        fprintf(stderr, "failed to attach\n");
        goto cleanup;
    }

    rb = ring_buffer__new(
        bpf_map__fd(skel->maps.events),
        handle_event,
        NULL,
        NULL);

    if (!rb)
    {
        fprintf(stderr, "failed to create ringbuf\n");
        goto cleanup;
    }

    printf("Tracing write() syscalls...\n");

    while (running)
    {
        int err = ring_buffer__poll(rb, 100);
        if (err < 0)
        {
            fprintf(stderr, "ring buffer poll error: %d\n", err);
            break;
        }

        static time_t last;
        time_t now = time(NULL);
        if (now > last && now - last >= 3)
        {
            last = now;
            print_stats();
        }
    }

cleanup:
    ring_buffer__free(rb);
    write_bpf__destroy(skel);

    return 0;
}