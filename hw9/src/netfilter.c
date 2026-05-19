#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/netfilter.h>
#include <linux/netfilter_ipv4.h>
#include <linux/ip.h>
#include <linux/tcp.h>
#include <linux/sysfs.h>
#include <linux/kobject.h>
#include <linux/slab.h>
#include <linux/string.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Alexey Smirnov");
MODULE_DESCRIPTION("TCP outgoing connections logger");

#define MAX_PORTS 16

static struct kobject *netfilter_kobj;
static u16 ports[MAX_PORTS];
static int ports_count = 0;

static ssize_t ports_store(struct kobject *kobj,
                           struct kobj_attribute *attr,
                           const char *buf,
                           size_t count)
{
    char *tmp, *token, *cur;
    ports_count = 0;

    if (!buf || buf[0] == '\0' || buf[0] == '\n')
        return count;

    tmp = kstrdup(buf, GFP_KERNEL);
    if (!tmp)
        return 0;

    cur = tmp;
    while ((token = strsep(&cur, ",\n")) != NULL)
    {
        unsigned long p;
        int ret;

        if (*token == '\0')
            continue;

        ret = kstrtoul(token, 10, &p);
        if (ret == 0 && p <= 65535)
        {
            ports[ports_count++] = (u16)p;
        }

        if (ports_count == MAX_PORTS)
        {
            pr_info("netfilter: too many ports\n");
            break;
        }
    }

    kfree(tmp);

    return count;
}

static ssize_t ports_show(struct kobject *kobj,
                          struct kobj_attribute *attr,
                          char *buf)
{
    int i, len = 0;

    for (i = 0; i < ports_count; i++)
    {
        len += sprintf(buf + len, "%u%s",
                       ports[i],
                       (i + 1 < ports_count) ? "," : "\n");
    }

    return len;
}

static struct kobj_attribute ports_attr =
    __ATTR(ports, 0664, ports_show, ports_store);

static unsigned int hook_func(void *priv,
                              struct sk_buff *skb,
                              const struct nf_hook_state *state)
{
    struct iphdr *ip_header;
    struct tcphdr *tcp_header;
    u16 dport;
    int i;

    if (!skb)
        return NF_ACCEPT;

    ip_header = ip_hdr(skb);
    if (!ip_header)
        return NF_ACCEPT;

    if (ip_header->protocol != IPPROTO_TCP)
        return NF_ACCEPT;

    tcp_header = tcp_hdr(skb);
    if (!tcp_header)
        return NF_ACCEPT;

    dport = ntohs(tcp_header->dest);

    for (i = 0; i < ports_count; i++)
    {
        if (dport == ports[i])
        {
            printk(KERN_INFO "TCP DROP: %pI4:%u -> %pI4:%u\n",
                   &ip_header->saddr,
                   ntohs(tcp_header->source),
                   &ip_header->daddr,
                   dport);

            return NF_DROP;
        }
    }

    if (tcp_header->syn &&
        !tcp_header->ack &&
        !tcp_header->rst)
    {
        printk(KERN_INFO "TCP CONNECTION: %pI4:%u -> %pI4:%u\n",
               &ip_header->saddr,
               ntohs(tcp_header->source),
               &ip_header->daddr,
               dport);
    }

    return NF_ACCEPT;
}

static struct nf_hook_ops nfho = {
    .hook = hook_func,
    .hooknum = NF_INET_LOCAL_OUT,
    .pf = PF_INET,
    .priority = NF_IP_PRI_FIRST};

int __init netfilter_init(void)
{
    int ret;

    netfilter_kobj = kobject_create_and_add("netfilter", kernel_kobj);
    if (!netfilter_kobj)
        return -ENOMEM;

    ret = sysfs_create_file(netfilter_kobj, &ports_attr.attr);
    if (ret)
    {
        kobject_put(netfilter_kobj);
        return ret;
    }

    nf_register_net_hook(&init_net, &nfho);

    printk(KERN_INFO "netfilter: loaded\n");
    return 0;
}

static void __exit netfilter_exit(void)
{
    kobject_put(netfilter_kobj);
    nf_unregister_net_hook(&init_net, &nfho);
    printk(KERN_INFO "netfilter: unloaded\n");
}

module_init(netfilter_init);
module_exit(netfilter_exit);