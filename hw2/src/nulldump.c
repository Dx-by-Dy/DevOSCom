#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h> // copy_from_user
#include <linux/slab.h>    // kmalloc, kfree
#include <linux/sched.h>   // current
#include <linux/cred.h>    // current_uid()

#define DEVICE_NAME "nulldump"
#define CLASS_NAME "nulldump_class"
#define MAX_DUMP_BYTES 256

static dev_t dev_number;
static struct cdev nulldump_cdev;
static struct class *nulldump_class;

static ssize_t nulldump_read(struct file *file, char __user *buf, size_t len, loff_t *off)
{
    pr_info("nulldump: comm=(%s) pid=%d uid=%u reading %zu bytes\n",
            current->comm,
            current->pid,
            current_uid().val,
            len);
    return 0;
}

static ssize_t nulldump_write(struct file *file,
                              const char __user *buf,
                              size_t count,
                              loff_t *ppos)
{
    size_t dump_len;
    unsigned char *kbuf;

    dump_len = min(count, (size_t)MAX_DUMP_BYTES);

    if (dump_len == 0)
        return 0;

    kbuf = kmalloc(dump_len, GFP_KERNEL);
    if (!kbuf)
        return -ENOMEM;

    if (copy_from_user(kbuf, buf, dump_len))
    {
        kfree(kbuf);
        return -EFAULT;
    }

    pr_info("nulldump: comm=(%s) pid=%d uid=%u writing %zu bytes:\n",
            current->comm,
            current->pid,
            current_uid().val,
            count);

    /**
     * print_hex_dump - print a text hex dump to syslog for a binary blob of data
     * @level: kernel log level (e.g. KERN_DEBUG)
     * @prefix_str: string to prefix each line with;
     *  caller supplies trailing spaces for alignment if desired
     * @prefix_type: controls whether prefix of an offset, address, or none
     *  is printed (%DUMP_PREFIX_OFFSET, %DUMP_PREFIX_ADDRESS, %DUMP_PREFIX_NONE)
     * @rowsize: number of bytes to print per line; must be 16 or 32
     * @groupsize: number of bytes to print at a time (1, 2, 4, 8; default = 1)
     * @buf: data blob to dump
     * @len: number of bytes in the @buf
     * @ascii: include ASCII after the hex output
     *
     * Given a buffer of u8 data, print_hex_dump() prints a hex + ASCII dump
     * to the kernel log at the specified kernel log level, with an optional
     * leading prefix.
     *
     * print_hex_dump() works on one "line" of output at a time, i.e.,
     * 16 or 32 bytes of input data converted to hex + ASCII output.
     * print_hex_dump() iterates over the entire input @buf, breaking it into
     * "line size" chunks to format and print.
     *
     * E.g.:
     *   print_hex_dump(KERN_DEBUG, "raw data: ", DUMP_PREFIX_ADDRESS,
     *		    16, 1, frame->data, frame->len, true);
     *
     * Example output using %DUMP_PREFIX_OFFSET and 1-byte mode:
     * 0009ab42: 40 41 42 43 44 45 46 47 48 49 4a 4b 4c 4d 4e 4f  @ABCDEFGHIJKLMNO
     * Example output using %DUMP_PREFIX_ADDRESS and 4-byte mode:
     * ffffffff88089af0: 73727170 77767574 7b7a7978 7f7e7d7c  pqrstuvwxyz{|}~.
     */
    print_hex_dump(KERN_INFO,
                   "data: ",
                   DUMP_PREFIX_OFFSET,
                   16,
                   1,
                   kbuf,
                   dump_len,
                   true);

    if (count > MAX_DUMP_BYTES)
        pr_info("nulldump: output truncated to %d bytes\n", MAX_DUMP_BYTES);

    kfree(kbuf);
    return count;
}

static const struct file_operations nulldump_fops = {
    .owner = THIS_MODULE,
    .write = nulldump_write,
    .read = nulldump_read,
};

static int __init nulldump_init(void)
{
    int ret;

    ret = alloc_chrdev_region(&dev_number, 0, 1, DEVICE_NAME);
    if (ret)
        return ret;

    cdev_init(&nulldump_cdev, &nulldump_fops);
    nulldump_cdev.owner = THIS_MODULE;

    ret = cdev_add(&nulldump_cdev, dev_number, 1);
    if (ret)
        goto err_unregister;

    nulldump_class = class_create(CLASS_NAME);
    if (IS_ERR(nulldump_class))
    {
        ret = PTR_ERR(nulldump_class);
        goto err_cdev;
    }

    if (IS_ERR(device_create(nulldump_class,
                             NULL,
                             dev_number,
                             NULL,
                             DEVICE_NAME)))
    {
        ret = -EINVAL;
        goto err_class;
    }

    pr_info("nulldump: module loaded\n");
    return 0;

err_class:
    class_destroy(nulldump_class);
err_cdev:
    cdev_del(&nulldump_cdev);
err_unregister:
    unregister_chrdev_region(dev_number, 1);
    return ret;
}

static void __exit nulldump_exit(void)
{
    device_destroy(nulldump_class, dev_number);
    class_destroy(nulldump_class);
    cdev_del(&nulldump_cdev);
    unregister_chrdev_region(dev_number, 1);
    pr_info("nulldump: module unloaded\n");
}

module_init(nulldump_init);
module_exit(nulldump_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Smirnov A.A.");
MODULE_DESCRIPTION("nulldump: /dev/null with hex dump logging");
MODULE_VERSION("1.0");