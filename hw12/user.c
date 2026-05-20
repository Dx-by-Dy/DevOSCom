#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <net/if.h>
#include <linux/if_link.h>

#include <bpf/libbpf.h>
#include <bpf/bpf.h>

#include "xdp_fw.skel.h"

static volatile int running = 1;

static void sig_handler(int signo)
{
    running = 0;
}

static void add_blocked_port(int map_fd, __u16 port)
{
    __u8 value = 1;
    if (bpf_map_update_elem(
            map_fd,
            &port,
            &value,
            BPF_ANY) != 0)
    {
        perror("bpf_map_update_elem error");
        exit(1);
    }
}

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        fprintf(stderr,
                "usage: %s IFACE PORT [PORT...]\n",
                argv[0]);
        return 1;
    }

    const char *ifname = argv[1];
    int ifindex = if_nametoindex(ifname);
    if (!ifindex)
    {
        perror("if_nametoindex error");
        return 1;
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    struct xdp_fw_bpf *skel;
    skel = xdp_fw_bpf__open_and_load();
    if (!skel)
    {
        fprintf(stderr, "failed to load skeleton\n");
        return 1;
    }

    int map_fd =
        bpf_map__fd(skel->maps.blocked_ports);

    for (int i = 2; i < argc; i++)
    {
        __u16 port = atoi(argv[i]);
        add_blocked_port(map_fd, port);
        printf("blocked port: %u\n", port);
    }

    if (bpf_xdp_attach(
            ifindex,
            bpf_program__fd(skel->progs.xdp_firewall),
            XDP_FLAGS_UPDATE_IF_NOEXIST,
            NULL))
    {
        fprintf(stderr, "failed to attach XDP\n");
        goto cleanup;
    }

    printf("XDP firewall attached to %s\n", ifname);
    while (running)
        sleep(1);

    bpf_xdp_detach(ifindex, XDP_FLAGS_UPDATE_IF_NOEXIST, NULL);

cleanup:
    xdp_fw_bpf__destroy(skel);

    return 0;
}