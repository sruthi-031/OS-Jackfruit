#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/sched/signal.h>
#include <linux/mm.h>
#include <linux/timer.h>
#include <linux/mutex.h>

#include "monitor_ioctl.h"

#define DEVICE_NAME "container_monitor"

static int major;
static struct class *cls;

/* ================= STRUCT ================= */
struct monitor_entry {
    pid_t pid;
    unsigned long soft_limit;
    unsigned long hard_limit;
    char container_id[32];
    int warned;
    struct list_head list;
};

static LIST_HEAD(monitor_list);
static DEFINE_MUTEX(monitor_lock);
static struct timer_list monitor_timer;

/* ================= GET RSS ================= */
static long get_rss_bytes(pid_t pid)
{
    struct task_struct *task;
    struct mm_struct *mm;
    long rss = -1;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (task) {
        mm = get_task_mm(task);
        if (mm) {
            rss = get_mm_rss(mm) << PAGE_SHIFT;
            mmput(mm);
        }
    }
    rcu_read_unlock();

    return rss;
}

/* ================= TIMER ================= */
static void timer_callback(struct timer_list *t)
{
    struct monitor_entry *entry, *tmp;

    mutex_lock(&monitor_lock);

    list_for_each_entry_safe(entry, tmp, &monitor_list, list) {

        long rss = get_rss_bytes(entry->pid);

        if (rss < 0) {
            list_del(&entry->list);
            kfree(entry);
            continue;
        }

        if (rss > entry->soft_limit && !entry->warned) {
            printk("[monitor] SOFT LIMIT: %s pid=%d rss=%ld limit=%lu\n",
                   entry->container_id, entry->pid, rss, entry->soft_limit);
            entry->warned = 1;
        }

        if (rss > entry->hard_limit) {
            printk("[monitor] HARD LIMIT: Killing %s pid=%d rss=%ld limit=%lu\n",
                   entry->container_id, entry->pid, rss, entry->hard_limit);

            kill_pid(find_vpid(entry->pid), SIGKILL, 1);

            list_del(&entry->list);
            kfree(entry);
        }
    }

    mutex_unlock(&monitor_lock);

    mod_timer(&monitor_timer, jiffies + HZ);
}

/* ================= IOCTL ================= */
static long device_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct monitor_request req;

    if (copy_from_user(&req, (void *)arg, sizeof(req)))
        return -EFAULT;

    if (cmd == MONITOR_REGISTER) {

        struct monitor_entry *entry;

        entry = kmalloc(sizeof(*entry), GFP_KERNEL);
        if (!entry)
            return -ENOMEM;

        entry->pid = req.pid;
        entry->soft_limit = req.soft_limit_bytes;
        entry->hard_limit = req.hard_limit_bytes;
        strncpy(entry->container_id, req.container_id, 31);
        entry->warned = 0;

        mutex_lock(&monitor_lock);
        list_add(&entry->list, &monitor_list);
        mutex_unlock(&monitor_lock);

        printk("[monitor] Registered pid=%d\n", req.pid);
    }

    return 0;
}

/* ================= FILE OPS ================= */
static struct file_operations fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = device_ioctl
};

/* ================= INIT ================= */
static int __init monitor_init(void)
{
    major = register_chrdev(0, DEVICE_NAME, &fops);
    cls = class_create( DEVICE_NAME);
    device_create(cls, NULL, MKDEV(major, 0), NULL, DEVICE_NAME);

    timer_setup(&monitor_timer, timer_callback, 0);
    mod_timer(&monitor_timer, jiffies + HZ);

    printk("Monitor module loaded\n");
    return 0;
}

/* ================= EXIT ================= */
static void __exit monitor_exit(void)
{
    del_timer_sync(&monitor_timer);

    device_destroy(cls, MKDEV(major, 0));
    class_destroy(cls);
    unregister_chrdev(major, DEVICE_NAME);

    printk("Monitor module unloaded\n");
}

module_init(monitor_init);
module_exit(monitor_exit);
MODULE_LICENSE("GPL");
