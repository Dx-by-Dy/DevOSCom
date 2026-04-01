#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/interrupt.h>
#include <linux/spinlock.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <asm/io.h>

MODULE_DESCRIPTION("Keyboard hook with timestamp");
MODULE_AUTHOR("Smirnov A.A.");
MODULE_LICENSE("GPL");

#define MODULE_NAME "kbd_hook"
#define KBD_MAJOR 0
#define KBD_NR_MINORS 1
#define I8042_KBD_IRQ 1
#define I8042_STATUS_REG 0x64
#define I8042_DATA_REG 0x60
#define SCANCODE_RELEASED_MASK 0x80

static dev_t dev_number;
static int max_entries = 1024;
module_param(max_entries, int, 0444);
MODULE_PARM_DESC(max_entries, "Max number of key events in buffer");

struct key_event
{
	char key;
	ktime_t timestamp;
};

struct kbd
{
	struct cdev cdev;
	spinlock_t lock;
	struct key_event *events;
	size_t put_idx, get_idx, count;
} kbd_ctx;

static int is_key_press(unsigned int scancode)
{
	return !(scancode & SCANCODE_RELEASED_MASK);
}

static int get_ascii(unsigned int scancode)
{
	static char *row1 = "1234567890";
	static char *row2 = "qwertyuiop";
	static char *row3 = "asdfghjkl";
	static char *row4 = "zxcvbnm";

	scancode &= ~SCANCODE_RELEASED_MASK;
	if (scancode >= 0x02 && scancode <= 0x0b)
		return row1[scancode - 0x02];
	if (scancode >= 0x10 && scancode <= 0x19)
		return row2[scancode - 0x10];
	if (scancode >= 0x1e && scancode <= 0x26)
		return row3[scancode - 0x1e];
	if (scancode >= 0x2c && scancode <= 0x32)
		return row4[scancode - 0x2c];
	if (scancode == 0x39)
		return ' ';
	if (scancode == 0x1c)
		return '\n';
	return '?';
}

static void put_event(struct kbd *data, char key)
{
	if (!data->events)
		return;

	data->events[data->put_idx].key = key;
	data->events[data->put_idx].timestamp = ktime_get();
	data->put_idx = (data->put_idx + 1) % max_entries;
	if (data->count < max_entries)
		data->count++;
	else
		data->get_idx = (data->get_idx + 1) % max_entries;
}

static bool get_event(struct key_event *ev, struct kbd *data, size_t idx)
{
	if (idx >= data->count)
		return false;
	*ev = data->events[(data->get_idx + idx) % max_entries];
	return true;
}

static inline u8 i8042_read_data(void)
{
	return inb(I8042_DATA_REG);
}

static irqreturn_t kbd_irq_handler(int irq, void *dev_id)
{
	struct kbd *data = (struct kbd *)dev_id;
	unsigned int scancode;
	int pressed, ch;

	scancode = i8042_read_data();
	pressed = is_key_press(scancode);
	ch = get_ascii(scancode);

	if (pressed)
	{
		unsigned long flags;
		spin_lock_irqsave(&data->lock, flags);
		put_event(data, ch);
		spin_unlock_irqrestore(&data->lock, flags);
	}

	// Ядро может вызвать Warning если постоянно кидать IRQ_NONE.
	return IRQ_HANDLED;
}

static int proc_show(struct seq_file *m, void *v)
{
	struct kbd *data = &kbd_ctx;
	unsigned long flags;
	size_t i;

	spin_lock_irqsave(&data->lock, flags);
	for (i = 0; i < data->count; i++)
	{
		struct key_event ev;
		if (get_event(&ev, data, i))
			seq_printf(m, "[%llu.%03llu] char: %c\n",
					   div_u64(ktime_to_ns(ev.timestamp), NSEC_PER_SEC),
					   div_u64(ktime_to_ns(ev.timestamp) % NSEC_PER_SEC, NSEC_PER_USEC),
					   ev.key);
	}
	spin_unlock_irqrestore(&data->lock, flags);
	return 0;
}

static int proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, proc_show, NULL);
}

static const struct proc_ops proc_fops = {
	.proc_open = proc_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static int kbd_open(struct inode *inode, struct file *file)
{
	struct kbd *data = container_of(inode->i_cdev, struct kbd, cdev);
	file->private_data = data;
	return 0;
}

static int kbd_release(struct inode *inode, struct file *file)
{
	return 0;
}

static ssize_t kbd_write(struct file *file, const char __user *buf,
						 size_t size, loff_t *offset)
{
	struct kbd *data = file->private_data;
	unsigned long flags;
	spin_lock_irqsave(&data->lock, flags);
	data->count = data->put_idx = data->get_idx = 0;
	spin_unlock_irqrestore(&data->lock, flags);
	return size;
}

static ssize_t kbd_read(struct file *file, char __user *buf,
						size_t size, loff_t *offset)
{
	return 0;
}

static const struct file_operations kbd_fops = {
	.owner = THIS_MODULE,
	.open = kbd_open,
	.release = kbd_release,
	.read = kbd_read,
	.write = kbd_write,
};

static int __init kbd_init(void)
{
	int ret;

	ret = alloc_chrdev_region(&dev_number, 0, KBD_NR_MINORS, MODULE_NAME);
	if (ret)
		return ret;

	cdev_init(&kbd_ctx.cdev, &kbd_fops);
	kbd_ctx.cdev.owner = THIS_MODULE;
	ret = cdev_add(&kbd_ctx.cdev, dev_number, 1);
	if (ret)
	{
		cdev_del(&kbd_ctx.cdev);
		unregister_chrdev_region(dev_number, KBD_NR_MINORS);
		return ret;
	}

	kbd_ctx.events = kcalloc(max_entries, sizeof(struct key_event), GFP_KERNEL);
	if (!kbd_ctx.events)
	{
		unregister_chrdev_region(dev_number, KBD_NR_MINORS);
		return -ENOMEM;
	}
	spin_lock_init(&kbd_ctx.lock);
	kbd_ctx.put_idx = kbd_ctx.get_idx = kbd_ctx.count = 0;

	if (!request_region(I8042_DATA_REG + 1, 1, MODULE_NAME) ||
		!request_region(I8042_STATUS_REG + 1, 1, MODULE_NAME))
	{
		kfree(kbd_ctx.events);
		cdev_del(&kbd_ctx.cdev);
		unregister_chrdev_region(dev_number, KBD_NR_MINORS);
		return -EBUSY;
	}

	ret = request_irq(I8042_KBD_IRQ, kbd_irq_handler, IRQF_SHARED, MODULE_NAME, &kbd_ctx);
	if (ret)
	{
		release_region(I8042_DATA_REG + 1, 1);
		release_region(I8042_STATUS_REG + 1, 1);
		kfree(kbd_ctx.events);
		cdev_del(&kbd_ctx.cdev);
		unregister_chrdev_region(dev_number, KBD_NR_MINORS);
		return ret;
	}

	proc_create(MODULE_NAME, 0444, NULL, &proc_fops);

	pr_info("kbd_hook loaded, buffer size %d\n", max_entries);
	return 0;
}

static void __exit kbd_exit(void)
{
	free_irq(I8042_KBD_IRQ, &kbd_ctx);
	release_region(I8042_DATA_REG + 1, 1);
	release_region(I8042_STATUS_REG + 1, 1);
	kfree(kbd_ctx.events);
	cdev_del(&kbd_ctx.cdev);
	remove_proc_entry(MODULE_NAME, NULL);
	unregister_chrdev_region(dev_number, KBD_NR_MINORS);

	pr_info("kbd_hook unloaded\n");
}

module_init(kbd_init);
module_exit(kbd_exit);