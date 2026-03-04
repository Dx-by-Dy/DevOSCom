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
#define CHUNK_LEN 256

static dev_t dev_number;
static struct cdev nulldump_cdev;
static struct class *nulldump_class;

static ssize_t nulldump_read(struct file *file, char __user *buf, size_t len, loff_t *off)
{
    pr_info("nulldump: comm=(%s) pid=%d uid=%u reading %zu bytes\n",
            current->comm,
            current->pid,
            __kuid_val(current_uid()),
            len);
    return 0;
}

static ssize_t nulldump_write(struct file *file,
                              const char __user *buf,
                              size_t count,
                              loff_t *ppos)
{
    size_t unprinted_len = count;
    unsigned char kbuf[CHUNK_LEN];

    pr_info("nulldump: comm=(%s) pid=%d uid=%u writing %zu bytes:\n",
            current->comm,
            current->pid,
            __kuid_val(current_uid()),
            count);

    while (unprinted_len > 0)
    {
        size_t chunk = min(unprinted_len, (size_t)CHUNK_LEN);
        if (copy_from_user(kbuf, buf + count - unprinted_len, chunk))
        {
            return -EFAULT;
        }
        print_hex_dump(KERN_INFO,
                       "data: ",
                       DUMP_PREFIX_OFFSET,
                       16,
                       1,
                       kbuf,
                       chunk,
                       true);
        unprinted_len -= chunk;
    }
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