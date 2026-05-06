#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/blkdev.h>
#include <linux/blk-mq.h>
#include <linux/hdreg.h>
#include <linux/vmalloc.h>
#include <linux/xarray.h>

#define DEVICE_NAME "ramdisk"
#define RAMDISK_SECTOR_SIZE 512
#define RAMDISK_PAGE_SIZE PAGE_SIZE

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Alexey Smirnov");
MODULE_DESCRIPTION("RAM Disk for Linux 6.16");

static int major;
static struct gendisk *ramdisk_gd;
static struct blk_mq_tag_set tag_set;
static unsigned long ramdisk_size = 16 * 1024 * 1024;
static sector_t ramdisk_sectors;
static DEFINE_XARRAY(ramdisk_pages);

static blk_status_t ramdisk_queue_rq(struct blk_mq_hw_ctx *hctx,
                                     const struct blk_mq_queue_data *bd)
{
    struct request *req = bd->rq;
    blk_status_t status = BLK_STS_OK;
    struct bio_vec bvec;
    struct req_iterator iter;
    unsigned long offset = blk_rq_pos(req) * RAMDISK_SECTOR_SIZE;

    blk_mq_start_request(req);

    rq_for_each_segment(bvec, req, iter)
    {
        unsigned int len = bvec.bv_len;
        if (offset + len > ramdisk_size)
        {
            status = BLK_STS_IOERR;
            break;
        }

        void *buffer = kmap_local_page(bvec.bv_page);
        unsigned long page_index = offset / RAMDISK_PAGE_SIZE;
        unsigned long page_offset = offset % RAMDISK_PAGE_SIZE;
        unsigned long buffer_offset = bvec.bv_offset;
        unsigned int current_len = min(len, RAMDISK_PAGE_SIZE - page_offset);
        while (current_len)
        {
            struct page *randisk_page = xa_load(&ramdisk_pages, page_index);
            if (!randisk_page)
            {
                if (rq_data_dir(req) == WRITE)
                {
                    randisk_page = alloc_page(GFP_KERNEL | __GFP_ZERO);
                    if (!randisk_page)
                    {
                        status = BLK_STS_IOERR;
                        break;
                    }
                    if (xa_is_err(xa_store(&ramdisk_pages, page_index, randisk_page, GFP_KERNEL)))
                    {
                        __free_page(randisk_page);
                        status = BLK_STS_IOERR;
                        break;
                    }
                }
                else
                {
                    memset(buffer + buffer_offset, 0, current_len);
                    goto next;
                }
            }

            void *ramdisk_buffer = kmap_local_page(randisk_page);
            if (rq_data_dir(req) == WRITE)
                memcpy(ramdisk_buffer + page_offset, buffer + buffer_offset, current_len);
            else
                memcpy(buffer + buffer_offset, ramdisk_buffer + page_offset, current_len);
            kunmap_local(ramdisk_buffer);

        next:
            offset += current_len;
            buffer_offset += current_len;
            page_index++;
            page_offset = 0;
            len -= current_len;
            current_len = min(len, RAMDISK_PAGE_SIZE);
        }

        kunmap_local(buffer);
    }

    blk_mq_end_request(req, status);
    return status;
}

static const struct blk_mq_ops ramdisk_mq_ops = {
    .queue_rq = ramdisk_queue_rq,
};

static const struct block_device_operations ramdisk_fops = {
    .owner = THIS_MODULE,
};

static int __init ramdisk_init(void)
{
    if (ramdisk_size == 0 || ramdisk_size % RAMDISK_SECTOR_SIZE != 0)
    {
        pr_err("ramdisk: invalid RAM disk size\n");
        return -EINVAL;
    }
    ramdisk_sectors = ramdisk_size / RAMDISK_SECTOR_SIZE;

    int ret;
    struct queue_limits lim = {
        .logical_block_size = RAMDISK_SECTOR_SIZE,
        .physical_block_size = RAMDISK_SECTOR_SIZE,
        .io_min = RAMDISK_SECTOR_SIZE,
        .io_opt = RAMDISK_PAGE_SIZE,
    };

    major = register_blkdev(0, DEVICE_NAME);
    if (major < 0)
    {
        return major;
    }

    memset(&tag_set, 0, sizeof(tag_set));
    tag_set.ops = &ramdisk_mq_ops;
    tag_set.nr_hw_queues = 1;
    tag_set.queue_depth = 128;
    tag_set.numa_node = NUMA_NO_NODE;
    tag_set.cmd_size = 0;
    tag_set.flags = 0;

    ret = blk_mq_alloc_tag_set(&tag_set);
    if (ret)
        goto out_unregister;

    ramdisk_gd = blk_mq_alloc_disk(&tag_set, &lim, NULL);
    if (IS_ERR(ramdisk_gd))
    {
        ret = PTR_ERR(ramdisk_gd);
        goto out_free_tagset;
    }

    snprintf(ramdisk_gd->disk_name, DISK_NAME_LEN, DEVICE_NAME);
    ramdisk_gd->fops = &ramdisk_fops;
    set_capacity(ramdisk_gd, ramdisk_sectors);

    ret = device_add_disk(NULL, ramdisk_gd, NULL);
    if (ret)
        goto out_put_disk;

    printk(KERN_INFO "ramdisk: %lu bytes RAM disk initialized\n",
           ramdisk_size);
    return 0;

out_put_disk:
    put_disk(ramdisk_gd);
out_free_tagset:
    blk_mq_free_tag_set(&tag_set);
out_unregister:
    unregister_blkdev(major, DEVICE_NAME);
    return ret;
}

static void __exit ramdisk_exit(void)
{
    struct page *randisk_page;
    unsigned long index;
    xa_for_each(&ramdisk_pages, index, randisk_page)
    {
        __free_page(randisk_page);
    }
    xa_destroy(&ramdisk_pages);

    del_gendisk(ramdisk_gd);
    put_disk(ramdisk_gd);
    blk_mq_free_tag_set(&tag_set);
    unregister_blkdev(major, DEVICE_NAME);
    printk(KERN_INFO "ramdisk: unloaded\n");
}

module_param(ramdisk_size, ulong, 0444);
MODULE_PARM_DESC(ramdisk_size, "RAM disk size in bytes");

module_init(ramdisk_init);
module_exit(ramdisk_exit);
