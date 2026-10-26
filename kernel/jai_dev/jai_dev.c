// jai_dev.c — JAI core kernel interface
//
// Creates a character device /dev/jai. This is JAI's first literal
// presence inside the kernel: userland (the jai-agent) opens /dev/jai
// to prove the kernel link, and can push short event strings that
// land in the kernel log (later phases: event ring buffer, wakeups).
//
// Build:  make            (from this directory)
// Load:   sudo insmod jai_dev.ko
// Test:   cat /dev/jai ; echo hello | tee /dev/jai ; dmesg | tail
// Unload: sudo rmmod jai_dev

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/atomic.h>

#define JAI_VERSION "0.1.0"

static atomic_t jai_open_count = ATOMIC_INIT(0);

static ssize_t jai_read(struct file *f, char __user *buf, size_t len, loff_t *off)
{
	static const char msg[] = "JAI is awake. Kernel link established.\n";

	return simple_read_from_buffer(buf, len, off, msg, sizeof(msg));
}

static ssize_t jai_write(struct file *f, const char __user *buf, size_t len, loff_t *off)
{
	char kbuf[64];
	size_t n = min(len, sizeof(kbuf) - 1);

	if (copy_from_user(kbuf, buf, n))
		return -EFAULT;
	kbuf[n] = '\0';
	/* Phase 3+ will replace the printk with a real event queue. */
	pr_info("jai: event: %s", kbuf);

	return len;
}

static int jai_open(struct inode *inode, struct file *file)
{
	atomic_inc(&jai_open_count);
	pr_info("jai: /dev/jai opened (total %d)\n", atomic_read(&jai_open_count));

	return 0;
}

static const struct file_operations jai_fops = {
	.owner	 = THIS_MODULE,
	.open	 = jai_open,
	.read	 = jai_read,
	.write	 = jai_write,
};

static struct miscdevice jai_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = "jai",
	.fops  = &jai_fops,
	.mode  = 0666,		/* agent runs as its own user */
};

static int __init jai_init(void)
{
	int ret = misc_register(&jai_misc);

	if (ret)
		return ret;
	pr_info("jai: core online v%s — /dev/jai ready\n", JAI_VERSION);

	return 0;
}

static void __exit jai_exit(void)
{
	misc_deregister(&jai_misc);
	pr_info("jai: core offline\n");
}

module_init(jai_init);
module_exit(jai_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("JAI Project");
MODULE_DESCRIPTION("JAI core kernel interface: /dev/jai");
MODULE_VERSION(JAI_VERSION);
