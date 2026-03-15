#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/kfifo.h>

#define DEVICE_NAME "pipebuf"
#define CLASS_NAME "pipebuf_class"
#define CHUNK_LEN 1024
#define FIFO_LEN 16384

static dev_t dev_number;
static struct cdev pipebuf_cdev;
static struct class *pipebuf_class;

static struct pipebuf_ctx
{
    struct kfifo fifo;
    atomic_t readers_open;
    atomic_t writers_open;
    spinlock_t lock;
    wait_queue_head_t readq;
    wait_queue_head_t writeq;
} pipebuf_ctx;

static ssize_t pipebuf_read(struct file *file, char __user *buf, size_t len, loff_t *off)
{
    unsigned int total_copied = 0;
    unsigned int read_len, copied;
    int ret;
    unsigned char tmp[CHUNK_LEN];

    while (total_copied < len)
    {
        ret = wait_event_interruptible(
            pipebuf_ctx.readq,
            kfifo_len(&pipebuf_ctx.fifo) > 0 || atomic_read(&pipebuf_ctx.writers_open) == 0);
        if (ret)
        {
            return ret;
        }

        while (1)
        {

            spin_lock(&pipebuf_ctx.lock);
            read_len = min(min(CHUNK_LEN, kfifo_len(&pipebuf_ctx.fifo)), (unsigned int)len - total_copied);
            if (!read_len)
            {
                spin_unlock(&pipebuf_ctx.lock);
                break;
            }
            copied = kfifo_out(&pipebuf_ctx.fifo, tmp, read_len);
            spin_unlock(&pipebuf_ctx.lock);

            if (copy_to_user(buf + total_copied, tmp, copied))
            {
                return -EFAULT;
            }

            total_copied += copied;
        }

        wake_up_interruptible(&pipebuf_ctx.writeq);

        if (atomic_read(&pipebuf_ctx.writers_open) == 0)
        {
            break;
        }
    }

    return total_copied;
}

static ssize_t pipebuf_write(struct file *file,
                             const char __user *buf,
                             size_t count,
                             loff_t *ppos)
{
    unsigned int total_copied = 0;
    unsigned int write_len, copied;
    int ret;
    unsigned char tmp[CHUNK_LEN];

    while (total_copied < count)
    {
        // Возможно стоит добавить "|| atomic_read(&pipebuf_ctx.reades_open) == 0"
        ret = wait_event_interruptible(pipebuf_ctx.writeq,
                                       kfifo_avail(&pipebuf_ctx.fifo) > 0);
        if (ret)
        {
            return ret;
        }

        // spin_lock(&pipebuf_ctx.lock);
        // ret = kfifo_from_user(&pipebuf_ctx.fifo,
        //                       buf + total_copied,
        //                       (unsigned int)count - total_copied,
        //                       &temp);
        // spin_unlock(&pipebuf_ctx.lock);

        while (1)
        {

            spin_lock(&pipebuf_ctx.lock);
            write_len = min(min(CHUNK_LEN, kfifo_avail(&pipebuf_ctx.fifo)), (unsigned int)count - total_copied);
            spin_unlock(&pipebuf_ctx.lock);
            if (!write_len)
            {
                break;
            }

            if (copy_from_user(tmp, buf + total_copied, write_len))
            {
                return -EFAULT;
            }

            spin_lock(&pipebuf_ctx.lock);
            copied = kfifo_in(&pipebuf_ctx.fifo, tmp, write_len);
            spin_unlock(&pipebuf_ctx.lock);

            total_copied += copied;
        }

        wake_up_interruptible(&pipebuf_ctx.readq);
    }

    return copied;
}

static int pipebuf_open(struct inode *inode, struct file *file)
{
    file->private_data = &pipebuf_ctx;

    if ((file->f_flags & O_ACCMODE) == O_RDONLY ||
        (file->f_flags & O_ACCMODE) == O_RDWR)
    {
        if (atomic_read(&pipebuf_ctx.readers_open) > 0)
            return -EBUSY;
        atomic_inc(&pipebuf_ctx.readers_open);
    }

    if ((file->f_flags & O_ACCMODE) == O_WRONLY ||
        (file->f_flags & O_ACCMODE) == O_RDWR)
    {
        atomic_inc(&pipebuf_ctx.writers_open);
    }

    return 0;
}

static int pipebuf_release(struct inode *inode, struct file *file)
{
    struct pipebuf_ctx *ctx = file->private_data;

    if ((file->f_flags & O_ACCMODE) == O_RDONLY ||
        (file->f_flags & O_ACCMODE) == O_RDWR)
    {
        atomic_dec(&ctx->readers_open);
        // wake_up_all(&ctx->writeq);
    }

    if ((file->f_flags & O_ACCMODE) == O_WRONLY ||
        (file->f_flags & O_ACCMODE) == O_RDWR)
    {
        atomic_dec(&ctx->writers_open);
        wake_up_all(&ctx->readq);
    }

    return 0;
}

static const struct file_operations pipebuf_fops = {
    .owner = THIS_MODULE,
    .write = pipebuf_write,
    .read = pipebuf_read,
    .open = pipebuf_open,
    .release = pipebuf_release,
};

static int __init pipebuf_init(void)
{
    int ret;

    atomic_set(&pipebuf_ctx.readers_open, 0);
    atomic_set(&pipebuf_ctx.writers_open, 0);
    spin_lock_init(&pipebuf_ctx.lock);
    init_waitqueue_head(&pipebuf_ctx.readq);
    init_waitqueue_head(&pipebuf_ctx.writeq);

    ret = kfifo_alloc(&pipebuf_ctx.fifo, 1024, GFP_KERNEL);
    if (ret)
    {
        return ret;
    }

    ret = alloc_chrdev_region(&dev_number, 0, 1, DEVICE_NAME);
    if (ret)
        return ret;

    cdev_init(&pipebuf_cdev, &pipebuf_fops);
    pipebuf_cdev.owner = THIS_MODULE;

    ret = cdev_add(&pipebuf_cdev, dev_number, 1);
    if (ret)
        goto err_unregister;

    pipebuf_class = class_create(CLASS_NAME);
    if (IS_ERR(pipebuf_class))
    {
        ret = PTR_ERR(pipebuf_class);
        goto err_cdev;
    }

    if (IS_ERR(device_create(pipebuf_class,
                             NULL,
                             dev_number,
                             NULL,
                             DEVICE_NAME)))
    {
        ret = -EINVAL;
        goto err_class;
    }

    pr_info("pipebuf: module loaded\n");
    return 0;

err_class:
    class_destroy(pipebuf_class);
err_cdev:
    cdev_del(&pipebuf_cdev);
err_unregister:
    unregister_chrdev_region(dev_number, 1);
    return ret;
}

static void __exit pipebuf_exit(void)
{
    wake_up_all(&pipebuf_ctx.readq);
    wake_up_all(&pipebuf_ctx.writeq);
    kfifo_free(&pipebuf_ctx.fifo);

    device_destroy(pipebuf_class, dev_number);
    class_destroy(pipebuf_class);
    cdev_del(&pipebuf_cdev);
    unregister_chrdev_region(dev_number, 1);
    pr_info("pipebuf: module unloaded\n");
}

module_init(pipebuf_init);
module_exit(pipebuf_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Smirnov A.A.");
MODULE_DESCRIPTION("pipebuf: module for a blocking read and write from fifo");
MODULE_VERSION("1.0");