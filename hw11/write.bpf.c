#include "vmlinux.h"

#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

#include "write.h"

char LICENSE[] SEC("license") = "GPL";

struct
{
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 65536);

    __type(key, __u64);
    __type(value, struct start_data);

} start SEC(".maps");

struct
{
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 1 << 24);

} events SEC(".maps");

SEC("tracepoint/syscalls/sys_enter_write")
int trace_enter_write(struct trace_event_raw_sys_enter *ctx)
{
    __u64 pid_tgid = bpf_get_current_pid_tgid();

    struct start_data data = {
        .ts = bpf_ktime_get_ns(),
    };

    bpf_map_update_elem(&start, &pid_tgid, &data, BPF_ANY);

    return 0;
}

SEC("tracepoint/syscalls/sys_exit_write")
int trace_exit_write(struct trace_event_raw_sys_exit *ctx)
{
    __u64 pid_tgid = bpf_get_current_pid_tgid();

    __u32 tgid = pid_tgid >> 32;
    __u32 tid = (__u32)pid_tgid;

    struct start_data *startp;

    startp = bpf_map_lookup_elem(&start, &pid_tgid);
    if (!startp)
        return 0;

    __u64 delta =
        bpf_ktime_get_ns() - startp->ts;

    struct event *e;

    e = bpf_ringbuf_reserve(&events, sizeof(*e), 0);
    if (!e)
        goto cleanup;

    e->tgid = tgid;
    e->tid = tid;

    e->latency_ns = delta;
    e->ret = ctx->ret;

    bpf_get_current_comm(&e->comm, sizeof(e->comm));

    bpf_ringbuf_submit(e, 0);

cleanup:
    bpf_map_delete_elem(&start, &pid_tgid);

    return 0;
}