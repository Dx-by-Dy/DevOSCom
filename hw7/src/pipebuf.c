#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/kfifo.h>
#include <linux/mutex.h>
#include <linux/slab.h>

#define DEVICE_NAME "pipebuf"
#define CLASS_NAME "pipebuf_class"
#define CHUNK_LEN 1024
#define START_FIFO_LEN 16384
#define MAX_DEVICES 16

static dev_t dev_number;
static struct class *pipebuf_class;
static DEFINE_MUTEX(global_mutex);
static unsigned int current_number_dev = 3;

struct pipebuf
{
    struct device *dev;
    struct cdev cdev;
    struct kfifo fifo;
    unsigned int current_fifo_len;
    atomic_t readers_open;
    atomic_t writers_open;
    spinlock_t lock;
    wait_queue_head_t readq;
    wait_queue_head_t writeq;
};

static struct pipebuf devices[MAX_DEVICES];

static const struct file_operations pipebuf_fops;

static ssize_t fifo_size_show(struct device *dev,
                              struct device_attribute *attr,
                              char *buf)
{
    struct pipebuf *ctx = dev_get_drvdata(dev);

    return sysfs_emit(buf, "%u\n", ctx->current_fifo_len);
}

static ssize_t fifo_size_store(struct device *dev,
                               struct device_attribute *attr,
                               const char *buf,
                               size_t count)
{
    struct pipebuf *ctx = dev_get_drvdata(dev);
    unsigned int new_size, copied;
    struct kfifo new_fifo;
    int ret;

    if (kstrtouint(buf, 10, &new_size))
        return -EINVAL;

    if ((new_size == 0) || ((new_size & (new_size - 1)) != 0))
        return -EINVAL;

    ret = kfifo_alloc(&new_fifo, new_size, GFP_KERNEL);
    if (ret)
        return ret;

    mutex_lock(&global_mutex);

    if (atomic_read(&ctx->readers_open) != 0 || atomic_read(&ctx->writers_open) != 0)
    {
        mutex_unlock(&global_mutex);
        kfifo_free(&new_fifo);
        return -EBUSY;
    }

    unsigned int len = kfifo_len(&ctx->fifo);
    unsigned char *tmp = kvmalloc(len, GFP_KERNEL);

    if (!tmp)
    {
        kfifo_free(&new_fifo);
        mutex_unlock(&global_mutex);
        return -ENOMEM;
    }

    copied = kfifo_out(&ctx->fifo, tmp, len);
    kfifo_in(&new_fifo, tmp, copied);

    kvfree(tmp);

    kfifo_free(&ctx->fifo);
    ctx->fifo = new_fifo;
    ctx->current_fifo_len = new_size;

    mutex_unlock(&global_mutex);

    wake_up_all(&ctx->writeq);

    return count;
}

static DEVICE_ATTR(fifo_size, 0664, fifo_size_show, fifo_size_store);

static ssize_t num_devices_show(const struct class *class,
                                const struct class_attribute *attr,
                                char *buf)
{
    return sysfs_emit(buf, "%u\n", current_number_dev);
}

static ssize_t num_devices_store(const struct class *class,
                                 const struct class_attribute *attr,
                                 const char *buf,
                                 size_t count)
{
    int ret, i;
    struct device *dev;
    unsigned int new_val;

    if (kstrtouint(buf, 10, &new_val))
        return -EINVAL;

    if (new_val > MAX_DEVICES)
        return -EINVAL;

    mutex_lock(&global_mutex);

    if (new_val > current_number_dev)
    {
        for (i = current_number_dev; i < new_val; i++)
        {
            atomic_set(&devices[i].readers_open, 0);
            atomic_set(&devices[i].writers_open, 0);
            spin_lock_init(&devices[i].lock);
            init_waitqueue_head(&devices[i].readq);
            init_waitqueue_head(&devices[i].writeq);
            devices[i].current_fifo_len = START_FIFO_LEN;

            ret = kfifo_alloc(&devices[i].fifo, START_FIFO_LEN, GFP_KERNEL);
            if (ret)
                goto err_fifo;
        }

        for (i = current_number_dev; i < new_val; i++)
        {
            cdev_init(&devices[i].cdev, &pipebuf_fops);
            devices[i].cdev.owner = THIS_MODULE;

            ret = cdev_add(&devices[i].cdev, MKDEV(MAJOR(dev_number), i), 1);
            if (ret)
            {
                goto err_cdev_add;
            }
        }

        for (i = current_number_dev; i < new_val; i++)
        {
            dev = device_create(pipebuf_class,
                                NULL,
                                MKDEV(MAJOR(dev_number), i),
                                NULL,
                                "pipebuf%d", i);
            if (IS_ERR(dev))
            {
                ret = -EINVAL;
                goto err_device_create;
            }
            devices[i].dev = dev;
            dev_set_drvdata(dev, &devices[i]);
        }

        for (i = current_number_dev; i < new_val; i++)
        {
            ret = device_create_file(devices[i].dev, &dev_attr_fifo_size);
            if (ret)
                goto err_device_create_file;
        }

        goto end;

    err_device_create_file:
        while (--i >= current_number_dev)
            device_remove_file(devices[i].dev, &dev_attr_fifo_size);
        i = new_val;
    err_device_create:
        while (--i >= current_number_dev)
            device_destroy(pipebuf_class, MKDEV(MAJOR(dev_number), i));
        i = new_val;
    err_cdev_add:
        while (--i >= current_number_dev)
            cdev_del(&devices[i].cdev);
        i = new_val;
    err_fifo:
        while (--i >= current_number_dev)
            kfifo_free(&devices[i].fifo);
        mutex_unlock(&global_mutex);
        return ret;
    }

    if (new_val < current_number_dev)
    {
        for (i = new_val; i < current_number_dev; i++)
        {
            if (atomic_read(&devices[i].readers_open) != 0 || atomic_read(&devices[i].writers_open) != 0)
            {
                mutex_unlock(&global_mutex);
                return -EBUSY;
            }
        }

        for (i = new_val; i < current_number_dev; i++)
        {
            device_destroy(pipebuf_class, MKDEV(MAJOR(dev_number), i));
            cdev_del(&devices[i].cdev);
            kfifo_free(&devices[i].fifo);
        }
    }

end:
    current_number_dev = new_val;
    mutex_unlock(&global_mutex);
    return count;
}

static CLASS_ATTR_RW(num_devices);

static ssize_t pipebuf_read(struct file *file, char __user *buf, size_t len, loff_t *off)
{
    struct pipebuf *pipebuf_ctx = file->private_data;
    unsigned int total_copied = 0;
    unsigned int read_len, copied;
    int ret;
    unsigned char tmp[CHUNK_LEN];

    while (total_copied < len)
    {
        ret = wait_event_interruptible(
            pipebuf_ctx->readq,
            kfifo_len(&pipebuf_ctx->fifo) > 0 || atomic_read(&pipebuf_ctx->writers_open) == 0);
        if (ret)
        {
            return ret;
        }

        unsigned int local_copied = 0;
        while (1)
        {

            spin_lock(&pipebuf_ctx->lock);
            read_len = min(min(CHUNK_LEN, kfifo_len(&pipebuf_ctx->fifo)), (unsigned int)len - total_copied);
            if (!read_len)
            {
                spin_unlock(&pipebuf_ctx->lock);
                break;
            }
            copied = kfifo_out(&pipebuf_ctx->fifo, tmp, read_len);
            spin_unlock(&pipebuf_ctx->lock);

            if (copy_to_user(buf + total_copied, tmp, copied))
            {
                return -EFAULT;
            }

            total_copied += copied;
            local_copied += copied;
        }

        if (local_copied)
            wake_up_all(&pipebuf_ctx->writeq);

        if (atomic_read(&pipebuf_ctx->writers_open) == 0)
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
    struct pipebuf *pipebuf_ctx = file->private_data;
    unsigned int total_copied = 0;
    unsigned int write_len, copied;
    int ret;
    unsigned char tmp[CHUNK_LEN];

    while (total_copied < count)
    {
        ret = wait_event_interruptible(pipebuf_ctx->writeq,
                                       kfifo_avail(&pipebuf_ctx->fifo) > 0);
        if (ret)
        {
            return ret;
        }

        unsigned int local_copied = 0;
        while (1)
        {

            spin_lock(&pipebuf_ctx->lock);
            write_len = min(min(CHUNK_LEN, kfifo_avail(&pipebuf_ctx->fifo)), (unsigned int)count - total_copied);
            spin_unlock(&pipebuf_ctx->lock);
            if (!write_len)
            {
                break;
            }

            if (copy_from_user(tmp, buf + total_copied, write_len))
            {
                return -EFAULT;
            }

            spin_lock(&pipebuf_ctx->lock);
            copied = kfifo_in(&pipebuf_ctx->fifo, tmp, write_len);
            spin_unlock(&pipebuf_ctx->lock);

            total_copied += copied;
            local_copied += copied;
        }

        if (local_copied)
            wake_up_all(&pipebuf_ctx->readq);
    }

    return total_copied;
}

static int pipebuf_open(struct inode *inode, struct file *file)
{
    int minor = iminor(inode);
    mutex_lock(&global_mutex);

    if (minor >= current_number_dev)
    {
        mutex_unlock(&global_mutex);
        return -ENODEV;
    }

    file->private_data = &devices[minor];

    if ((file->f_flags & O_ACCMODE) == O_RDONLY ||
        (file->f_flags & O_ACCMODE) == O_RDWR)
    {
        if (atomic_cmpxchg(&devices[minor].readers_open, 0, 1) != 0)
        {
            mutex_unlock(&global_mutex);
            return -EBUSY;
        }
    }

    if ((file->f_flags & O_ACCMODE) == O_WRONLY ||
        (file->f_flags & O_ACCMODE) == O_RDWR)
    {
        atomic_inc(&devices[minor].writers_open);
    }

    mutex_unlock(&global_mutex);

    return 0;
}

static int pipebuf_release(struct inode *inode, struct file *file)
{
    struct pipebuf *pipebuf_ctx = file->private_data;
    mutex_lock(&global_mutex);

    if ((file->f_flags & O_ACCMODE) == O_RDONLY ||
        (file->f_flags & O_ACCMODE) == O_RDWR)
    {
        atomic_dec(&pipebuf_ctx->readers_open);
        // wake_up_all(&pipebuf_ctx->writeq);
    }

    if ((file->f_flags & O_ACCMODE) == O_WRONLY ||
        (file->f_flags & O_ACCMODE) == O_RDWR)
    {
        atomic_dec(&pipebuf_ctx->writers_open);
        wake_up_all(&pipebuf_ctx->readq);
    }

    mutex_unlock(&global_mutex);

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
    int ret, i;
    struct device *dev;

    ret = alloc_chrdev_region(&dev_number, 0, MAX_DEVICES, DEVICE_NAME);
    if (ret)
        return ret;

    pipebuf_class = class_create(CLASS_NAME);
    if (IS_ERR(pipebuf_class))
    {
        ret = PTR_ERR(pipebuf_class);
        goto err_class_create;
    }
    ret = class_create_file(pipebuf_class, &class_attr_num_devices);
    if (ret)
        goto err_class_file;

    for (i = 0; i < current_number_dev; i++)
    {
        atomic_set(&devices[i].readers_open, 0);
        atomic_set(&devices[i].writers_open, 0);
        spin_lock_init(&devices[i].lock);
        init_waitqueue_head(&devices[i].readq);
        init_waitqueue_head(&devices[i].writeq);
        devices[i].current_fifo_len = START_FIFO_LEN;

        ret = kfifo_alloc(&devices[i].fifo, START_FIFO_LEN, GFP_KERNEL);
        if (ret)
            goto err_fifo;
    }

    for (i = 0; i < current_number_dev; i++)
    {
        cdev_init(&devices[i].cdev, &pipebuf_fops);
        devices[i].cdev.owner = THIS_MODULE;

        ret = cdev_add(&devices[i].cdev, MKDEV(MAJOR(dev_number), i), 1);
        if (ret)
        {
            goto err_cdev_add;
        }
    }

    for (i = 0; i < current_number_dev; i++)
    {
        dev = device_create(pipebuf_class,
                            NULL,
                            MKDEV(MAJOR(dev_number), i),
                            NULL,
                            "pipebuf%d", i);
        if (IS_ERR(dev))
        {
            ret = -EINVAL;
            goto err_device_create;
        }
        devices[i].dev = dev;
        dev_set_drvdata(dev, &devices[i]);
    }

    for (i = 0; i < current_number_dev; i++)
    {
        ret = device_create_file(devices[i].dev, &dev_attr_fifo_size);
        if (ret)
            goto err_device_create_file;
    }

    pr_info("pipebuf: module loaded\n");
    return 0;

err_device_create_file:
    while (--i >= 0)
        device_remove_file(devices[i].dev, &dev_attr_fifo_size);
    i = current_number_dev;
err_device_create:
    while (--i >= 0)
        device_destroy(pipebuf_class, MKDEV(MAJOR(dev_number), i));
    i = current_number_dev;
err_cdev_add:
    while (--i >= 0)
        cdev_del(&devices[i].cdev);
    i = current_number_dev;
err_fifo:
    while (--i >= 0)
        kfifo_free(&devices[i].fifo);
err_class_file:
    class_destroy(pipebuf_class);
err_class_create:
    unregister_chrdev_region(dev_number, MAX_DEVICES);
    return ret;
}

static void __exit pipebuf_exit(void)
{
    for (int i = 0; i < current_number_dev; i++)
    {
        device_remove_file(devices[i].dev, &dev_attr_fifo_size);
        device_destroy(pipebuf_class, MKDEV(MAJOR(dev_number), i));
        cdev_del(&devices[i].cdev);
        kfifo_free(&devices[i].fifo);
    }

    class_remove_file(pipebuf_class, &class_attr_num_devices);
    class_destroy(pipebuf_class);
    unregister_chrdev_region(dev_number, MAX_DEVICES);
    pr_info("pipebuf: module unloaded\n");
}

module_init(pipebuf_init);
module_exit(pipebuf_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Smirnov A.A.");
MODULE_DESCRIPTION("pipebuf: module for a blocking read and write from fifo");
MODULE_VERSION("2.0");