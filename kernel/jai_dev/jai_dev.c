// jai_dev.c — JAI core kernel interface (Phase 3: event ring buffer)
//
// Creates the character device /dev/jai, JAI's literal presence inside
// the kernel. Semantics, v0.2.0:
//
//   write(fd, "text")   -> event pushed into an in-kernel ring buffer
//                          AND mirrored to the kernel log ("jai: event: ...")
//                          so `journalctl -k` keeps working as a journal.
//   read(fd)            -> drains PENDING events (oldest first).
//                          When the buffer is empty, returns a status line:
//                            "JAI is awake. Kernel link established."
//                            "JAI core v0.2.0 | events: N total | opens: M"
//                          so `cat /dev/jai` and the agent's jai_kernel_status
//                          MCP tool stay meaningful at any moment.
//
//   - Buffer: 4 KiB static ring, oldest bytes overwritten when full.
//   - Events: up to 254 bytes per write(); a trailing newline is added
//     if missing so `cat` output stays line-oriented.
//   - read() is a drain (pipe-like): *ppos is not advanced for event
//     bytes; the empty-buffer status uses ppos so readers terminate.
//     Single-reader design assumed (the JAI agent).
//
// Build:  make            (from this directory)
// Load:   sudo insmod jai_dev.ko
// Test:   cat /dev/jai ; echo hello | tee /dev/jai ; cat /dev/jai ; cat /dev/jai
//         (1st cat drains "hello", 2nd shows the status line)
// Unload: sudo rmmod jai_dev

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/atomic.h>
#include <linux/mutex.h>
#include <linux/minmax.h>

#define JAI_VERSION "0.2.0"

/* --- event ring buffer ------------------------------------------------- */
#define JAI_RB_SIZE  4096                     /* bytes, power of two      */
#define JAI_RB_MASK  (JAI_RB_SIZE - 1)
#define JAI_EVENT_MAX 254                     /* per-write cap            */

static char     jai_rb[JAI_RB_SIZE];
static size_t   jai_rb_head;                  /* next byte to write       */
static size_t   jai_rb_tail;                  /* next byte to read        */
static size_t   jai_rb_count;                 /* pending bytes            */
static DEFINE_MUTEX(jai_rb_lock);

static atomic64_t jai_events_total = ATOMIC64_INIT(0);
static atomic_t   jai_open_count   = ATOMIC_INIT(0);

static void jai_rb_push(const char *s, size_t n)
{
        mutex_lock(&jai_rb_lock);
        while (n--) {
                jai_rb[jai_rb_head] = *s++;
                jai_rb_head = (jai_rb_head + 1) & JAI_RB_MASK;
                if (jai_rb_count < JAI_RB_SIZE)
                        jai_rb_count++;
                else
                        jai_rb_tail = (jai_rb_tail + 1) & JAI_RB_MASK; /* full: drop oldest */
        }
        mutex_unlock(&jai_rb_lock);
}

/* Copy up to len pending bytes to userspace. Returns bytes copied or -EFAULT. */
static ssize_t jai_rb_drain(char __user *buf, size_t len)
{
        size_t want, first, rest, drained = 0;

        mutex_lock(&jai_rb_lock);
        want = min(len, jai_rb_count);
        if (want == 0) {
                mutex_unlock(&jai_rb_lock);
                return 0;
        }
        /* ring may wrap: copy the tail->end segment, then the 0-> segment */
        first = min(want, (size_t)JAI_RB_SIZE - jai_rb_tail);
        rest  = want - first;

        if (copy_to_user(buf, jai_rb + jai_rb_tail, first)) {
                mutex_unlock(&jai_rb_lock);
                return -EFAULT;
        }
        drained = first;
        if (rest && copy_to_user(buf + first, jai_rb, rest)) {
                mutex_unlock(&jai_rb_lock);
                return -EFAULT;
        }
        drained += rest;

        jai_rb_tail  = (jai_rb_tail + drained) & JAI_RB_MASK;
        jai_rb_count -= drained;
        mutex_unlock(&jai_rb_lock);

        return drained;
}

/* --- file operations ---------------------------------------------------- */
static ssize_t jai_read(struct file *f, char __user *buf, size_t len, loff_t *off)
{
        char status[128];
        int  slen;

        if (jai_rb_count > 0) {
                ssize_t n = jai_rb_drain(buf, len);
                if (n < 0)
                        return n;
                if (n > 0)
                        return n;          /* drained (another reader may */
        }                                  /* have won the race — fall through */

        slen = scnprintf(status, sizeof(status),
                         "JAI is awake. Kernel link established.\n"
                         "JAI core v%s | events: %llu total | opens: %d\n",
                         JAI_VERSION, atomic64_read(&jai_events_total),
                         atomic_read(&jai_open_count));
        return simple_read_from_buffer(buf, len, off, status, slen);
}

static ssize_t jai_write(struct file *f, const char __user *buf, size_t len, loff_t *off)
{
        char kbuf[JAI_EVENT_MAX + 2];
        size_t n;

        if (len == 0)
                return 0;
        n = min(len, (size_t)JAI_EVENT_MAX);
        if (copy_from_user(kbuf, buf, n))
                return -EFAULT;
        kbuf[n] = '\0';
        if (kbuf[n - 1] != '\n') {         /* keep cat output line-oriented */
                kbuf[n++] = '\n';
                kbuf[n]   = '\0';
        }

        jai_rb_push(kbuf, n);
        atomic64_inc(&jai_events_total);
        pr_info("jai: event: %s", kbuf);   /* kernel-log mirror (journalctl -k) */

        return len;                        /* whole payload accepted */
}

static int jai_open(struct inode *inode, struct file *file)
{
        atomic_inc(&jai_open_count);
        pr_info("jai: /dev/jai opened (total %d)\n", atomic_read(&jai_open_count));

        return 0;
}

static const struct file_operations jai_fops = {
        .owner   = THIS_MODULE,
        .open    = jai_open,
        .read    = jai_read,
        .write   = jai_write,
};

static struct miscdevice jai_misc = {
        .minor = MISC_DYNAMIC_MINOR,
        .name  = "jai",
        .fops  = &jai_fops,
        .mode  = 0666,          /* agent runs as its own user */
};

static int __init jai_init(void)
{
        int ret = misc_register(&jai_misc);

        if (ret)
                return ret;
        pr_info("jai: core online v%s — /dev/jai ready (event ring %d B)\n",
                JAI_VERSION, JAI_RB_SIZE);

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
MODULE_DESCRIPTION("JAI core kernel interface: /dev/jai with event ring buffer");
MODULE_VERSION(JAI_VERSION);
