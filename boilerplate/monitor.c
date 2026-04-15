/*
 * monitor.c — Multi-Container Memory Monitor (Linux Kernel Module)
 *
 * Full implementation.
 *
 * Design:
 *   - A singly-linked kernel list (struct list_head) tracks all registered
 *     container PIDs with their soft and hard memory limits.
 *   - A mutex protects the list from concurrent ioctl() and timer() access.
 *     A mutex is chosen over a spinlock because both code paths (ioctl and
 *     timer callback) may sleep (e.g., kzalloc with GFP_KERNEL), making
 *     a spinlock inappropriate.
 *   - A kernel timer fires every CHECK_INTERVAL_SEC seconds.  For each
 *     entry it reads the process's RSS via get_rss_bytes():
 *       >= hard limit  → SIGKILL, remove entry
 *       >= soft limit  → printk warning once per entry
 *       process gone   → remove stale entry
 *   - MONITOR_REGISTER ioctl  → allocate and insert a new entry
 *   - MONITOR_UNREGISTER ioctl → find and remove an existing entry
 *   - module_exit()            → free all remaining entries
 */

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pid.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/uaccess.h>
#include <linux/version.h>

/* Kernel 6.15+ renamed del_timer_sync to timer_delete_sync */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 15, 0)
#define timer_delete_sync(t) del_timer_sync(t)
#endif

#include "monitor_ioctl.h"

#define DEVICE_NAME        "container_monitor"
#define CHECK_INTERVAL_SEC 1

/* ═══════════════════════════════════════════════════════════════════
 * TODO 1: Linked-list node struct
 *
 * Tracks one container process:
 *   pid              — host PID to monitor
 *   container_id     — human-readable name for log messages
 *   soft_limit_bytes — threshold that triggers a warning
 *   hard_limit_bytes — threshold that triggers SIGKILL
 *   soft_warned      — set after the first soft-limit warning so we
 *                       only log it once per entry (not every tick)
 *   list             — kernel intrusive list linkage
 * ═══════════════════════════════════════════════════════════════════ */
struct monitored_entry {
    pid_t           pid;
    char            container_id[MONITOR_NAME_LEN];
    unsigned long   soft_limit_bytes;
    unsigned long   hard_limit_bytes;
    int             soft_warned;
    struct list_head list;
};

/* ═══════════════════════════════════════════════════════════════════
 * TODO 2: Global list head and mutex
 *
 * Why mutex over spinlock:
 *   Both code paths that touch the list (ioctl and timer_callback)
 *   may call kzalloc(GFP_KERNEL), which can sleep.  Spinlocks must
 *   not be held while sleeping.  A mutex is the correct primitive.
 *
 * Race conditions without this lock:
 *   - ioctl REGISTER could add a node while the timer iterates → the
 *     timer might traverse a partially-linked node (corrupted next ptr).
 *   - ioctl UNREGISTER could free a node the timer is currently reading
 *     → use-after-free in the timer.
 *   - Two concurrent REGISTER ioctls could both set list_add_tail on
 *     overlapping prev/next pointers → list corruption.
 * ═══════════════════════════════════════════════════════════════════ */
static LIST_HEAD(monitored_list);
static DEFINE_MUTEX(list_mutex);

/* ─── Provided: internal device / timer state ─────────────────── */
static struct timer_list monitor_timer;
static dev_t             dev_num;
static struct cdev       c_dev;
static struct class     *cl;

/* ---------------------------------------------------------------
 * Provided: RSS helper
 * Returns the Resident Set Size in bytes for the given PID,
 * or -1 if the task no longer exists.
 * --------------------------------------------------------------- */
static long get_rss_bytes(pid_t pid)
{
    struct task_struct *task;
    struct mm_struct   *mm;
    long rss_pages = 0;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (!task) {
        rcu_read_unlock();
        return -1;
    }
    get_task_struct(task);
    rcu_read_unlock();

    mm = get_task_mm(task);
    if (mm) {
        rss_pages = get_mm_rss(mm);
        mmput(mm);
    }
    put_task_struct(task);

    return rss_pages * PAGE_SIZE;
}

/* ---------------------------------------------------------------
 * Provided: soft-limit event logger
 * --------------------------------------------------------------- */
static void log_soft_limit_event(const char *container_id,
                                  pid_t pid,
                                  unsigned long limit_bytes,
                                  long rss_bytes)
{
    printk(KERN_WARNING
           "[container_monitor] SOFT LIMIT container=%s pid=%d"
           " rss=%ld limit=%lu\n",
           container_id, pid, rss_bytes, limit_bytes);
}

/* ---------------------------------------------------------------
 * Provided: hard-limit enforcer — sends SIGKILL to the process
 * --------------------------------------------------------------- */
static void kill_process(const char *container_id,
                          pid_t pid,
                          unsigned long limit_bytes,
                          long rss_bytes)
{
    struct task_struct *task;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (task)
        send_sig(SIGKILL, task, 1);
    rcu_read_unlock();

    printk(KERN_WARNING
           "[container_monitor] HARD LIMIT container=%s pid=%d"
           " rss=%ld limit=%lu — SIGKILL sent\n",
           container_id, pid, rss_bytes, limit_bytes);
}

/* ═══════════════════════════════════════════════════════════════════
 * TODO 3: Periodic monitoring timer callback
 *
 * Fires every CHECK_INTERVAL_SEC seconds.
 *
 * Iteration strategy:
 *   list_for_each_entry_safe() is used so we can safely call
 *   list_del() + kfree() on the current entry while iterating.
 *   The 'tmp' cursor saves the next pointer before we potentially
 *   free the current node, preventing use-after-free.
 *
 * Per-entry policy:
 *   1. get_rss_bytes() < 0  →  process exited; remove stale entry.
 *   2. RSS >= hard limit     →  kill, then remove entry.
 *   3. RSS >= soft limit
 *      and not yet warned    →  emit soft-limit warning, set flag.
 *      (flag prevents repeated warnings for the same container)
 * ═══════════════════════════════════════════════════════════════════ */
static void timer_callback(struct timer_list *t)
{
    struct monitored_entry *entry, *tmp;
    long rss;

    mutex_lock(&list_mutex);

    list_for_each_entry_safe(entry, tmp, &monitored_list, list) {
        rss = get_rss_bytes(entry->pid);

        /* Process no longer exists — remove stale entry */
        if (rss < 0) {
            printk(KERN_INFO
                   "[container_monitor] container=%s pid=%d exited,"
                   " removing from watch list\n",
                   entry->container_id, entry->pid);
            list_del(&entry->list);
            kfree(entry);
            continue;
        }

        /* Hard limit: kill and remove */
        if ((unsigned long)rss >= entry->hard_limit_bytes) {
            kill_process(entry->container_id, entry->pid,
                         entry->hard_limit_bytes, rss);
            list_del(&entry->list);
            kfree(entry);
            continue;
        }

        /* Soft limit: warn once */
        if ((unsigned long)rss >= entry->soft_limit_bytes &&
            !entry->soft_warned) {
            log_soft_limit_event(entry->container_id, entry->pid,
                                 entry->soft_limit_bytes, rss);
            entry->soft_warned = 1;
        }
    }

    mutex_unlock(&list_mutex);

    /* Re-arm timer */
    mod_timer(&monitor_timer, jiffies + CHECK_INTERVAL_SEC * HZ);
}

/* ═══════════════════════════════════════════════════════════════════
 * IOCTL handler
 *
 * MONITOR_REGISTER   — add a new monitored entry
 * MONITOR_UNREGISTER — remove an existing entry by PID
 * ═══════════════════════════════════════════════════════════════════ */
static long monitor_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    struct monitor_request req;

    (void)f;

    if (cmd != MONITOR_REGISTER && cmd != MONITOR_UNREGISTER)
        return -EINVAL;

    if (copy_from_user(&req, (struct monitor_request __user *)arg,
                       sizeof(req)))
        return -EFAULT;

    /* ── TODO 4: REGISTER ─────────────────────────────────────────
     * Allocate a new monitored_entry, initialise it from req, and
     * append it to the monitored_list under list_mutex.
     * ─────────────────────────────────────────────────────────── */
    if (cmd == MONITOR_REGISTER) {
        struct monitored_entry *entry;

        printk(KERN_INFO
               "[container_monitor] Registering container=%s pid=%d"
               " soft=%lu hard=%lu\n",
               req.container_id, req.pid,
               req.soft_limit_bytes, req.hard_limit_bytes);

        if (req.soft_limit_bytes > req.hard_limit_bytes) {
            printk(KERN_WARNING
                   "[container_monitor] soft > hard limit for pid=%d,"
                   " rejecting\n", req.pid);
            return -EINVAL;
        }

        entry = kzalloc(sizeof(*entry), GFP_KERNEL);
        if (!entry)
            return -ENOMEM;

        entry->pid              = req.pid;
        entry->soft_limit_bytes = req.soft_limit_bytes;
        entry->hard_limit_bytes = req.hard_limit_bytes;
        entry->soft_warned      = 0;
        strncpy(entry->container_id, req.container_id,
                MONITOR_NAME_LEN - 1);
        INIT_LIST_HEAD(&entry->list);

        mutex_lock(&list_mutex);
        list_add_tail(&entry->list, &monitored_list);
        mutex_unlock(&list_mutex);

        return 0;
    }

    /* ── TODO 5: UNREGISTER ───────────────────────────────────────
     * Find the entry by PID, remove it from the list, and free it.
     * Return -ENOENT if no matching entry is found.
     * ─────────────────────────────────────────────────────────── */
    printk(KERN_INFO
           "[container_monitor] Unregister request container=%s pid=%d\n",
           req.container_id, req.pid);

    {
        struct monitored_entry *entry, *tmp;
        int found = 0;

        mutex_lock(&list_mutex);
        list_for_each_entry_safe(entry, tmp, &monitored_list, list) {
            if (entry->pid == req.pid) {
                list_del(&entry->list);
                kfree(entry);
                found = 1;
                break;
            }
        }
        mutex_unlock(&list_mutex);

        if (!found) {
            printk(KERN_WARNING
                   "[container_monitor] unregister: pid=%d not found\n",
                   req.pid);
            return -ENOENT;
        }
        return 0;
    }
}

/* ─── Provided: file operations ─────────────────────────────────── */
static struct file_operations fops = {
    .owner          = THIS_MODULE,
    .unlocked_ioctl = monitor_ioctl,
};

/* ─── Provided: Module init ─────────────────────────────────────── */
static int __init monitor_init(void)
{
    if (alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME) < 0)
        return -1;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
    cl = class_create(DEVICE_NAME);
#else
    cl = class_create(THIS_MODULE, DEVICE_NAME);
#endif
    if (IS_ERR(cl)) {
        unregister_chrdev_region(dev_num, 1);
        return PTR_ERR(cl);
    }

    if (IS_ERR(device_create(cl, NULL, dev_num, NULL, DEVICE_NAME))) {
        class_destroy(cl);
        unregister_chrdev_region(dev_num, 1);
        return -1;
    }

    cdev_init(&c_dev, &fops);
    if (cdev_add(&c_dev, dev_num, 1) < 0) {
        device_destroy(cl, dev_num);
        class_destroy(cl);
        unregister_chrdev_region(dev_num, 1);
        return -1;
    }

    timer_setup(&monitor_timer, timer_callback, 0);
    mod_timer(&monitor_timer, jiffies + CHECK_INTERVAL_SEC * HZ);

    printk(KERN_INFO
           "[container_monitor] Module loaded. Device: /dev/%s\n",
           DEVICE_NAME);
    return 0;
}

/* ─── Provided: Module exit ─────────────────────────────────────── */
static void __exit monitor_exit(void)
{
    timer_delete_sync(&monitor_timer);

    /* ── TODO 6: Free all remaining monitored entries ─────────────
     * At module unload there may be containers still registered
     * (e.g., if the supervisor crashed).  Walk the list and free
     * every node to prevent kernel memory leaks.
     * ─────────────────────────────────────────────────────────── */
    {
        struct monitored_entry *entry, *tmp;

        mutex_lock(&list_mutex);
        list_for_each_entry_safe(entry, tmp, &monitored_list, list) {
            printk(KERN_INFO
                   "[container_monitor] cleanup: freeing container=%s pid=%d\n",
                   entry->container_id, entry->pid);
            list_del(&entry->list);
            kfree(entry);
        }
        mutex_unlock(&list_mutex);
    }

    cdev_del(&c_dev);
    device_destroy(cl, dev_num);
    class_destroy(cl);
    unregister_chrdev_region(dev_num, 1);

    printk(KERN_INFO "[container_monitor] Module unloaded.\n");
}

module_init(monitor_init);
module_exit(monitor_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Supervised multi-container memory monitor");
MODULE_AUTHOR("Manoj PS — PES1UG24CS265");
