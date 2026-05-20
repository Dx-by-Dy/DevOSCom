//go:build ignore

#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/in.h>
#include <linux/tcp.h>
#include <linux/udp.h>

#include <bpf/bpf_helpers.h>

char LICENSE[] SEC("license") = "GPL";

struct {
    __uint(type, BPF_MAP_TYPE_HASH);

    __uint(max_entries, 1024);

    __type(key, __u16);
    __type(value, __u8);

} blocked_ports SEC(".maps");

SEC("xdp")
int xdp_firewall(struct xdp_md *ctx)
{
    void *data_end = (void *)(long)ctx->data_end;
    void *data = (void *)(long)ctx->data;
    struct ethhdr *eth = data;

    if ((void *)(eth + 1) > data_end)
        return XDP_PASS;

    if (eth->h_proto != __constant_htons(ETH_P_IP))
        return XDP_PASS;

    struct iphdr *ip = (void *)(eth + 1);

    if ((void *)(ip + 1) > data_end)
        return XDP_PASS;

    if (ip->ihl < 5)
        return XDP_PASS;

    __u32 ip_hdr_len = ip->ihl * 4;
    if ((void *)ip + ip_hdr_len > data_end)
        return XDP_PASS;

    __u16 dport = 0;
    if (ip->protocol == IPPROTO_TCP) {
        struct tcphdr *tcp =
            (void *)ip + ip_hdr_len;
        if ((void *)(tcp + 1) > data_end)
            return XDP_PASS;
        dport = __builtin_bswap16(tcp->dest);

    } else if (ip->protocol == IPPROTO_UDP) {
        struct udphdr *udp =
            (void *)ip + ip_hdr_len;
        if ((void *)(udp + 1) > data_end)
            return XDP_PASS;
        dport = __builtin_bswap16(udp->dest);

    } else {
        return XDP_PASS;
    }

    __u8 *blocked;
    blocked =
        bpf_map_lookup_elem(&blocked_ports, &dport);
    if (blocked)
        return XDP_DROP;

    return XDP_PASS;
}