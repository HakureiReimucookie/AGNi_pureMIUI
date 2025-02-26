/* drivers/misc/uid_cputime.c
 *
 * Copyright (C) 2014 - 2015 Google, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/atomic.h>
#include <linux/err.h>
#include <linux/hashtable.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/proc_fs.h>
#include <linux/profile.h>
#include <linux/rtmutex.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/rtmutex.h>
#include <linux/cpufreq.h>

#define UID_HASH_BITS	10
static DECLARE_HASHTABLE(hash_table, UID_HASH_BITS);

static DEFINE_RT_MUTEX(uid_lock);
static struct proc_dir_entry *cpu_parent;
static struct proc_dir_entry *io_parent;
static struct proc_dir_entry *proc_parent;

struct io_stats {
	u64 read_bytes;
	u64 write_bytes;
	u64 rchar;
	u64 wchar;
	u64 fsync;
};

#define UID_STATE_FOREGROUND	0
#define UID_STATE_BACKGROUND	1
#define UID_STATE_BUCKET_SIZE	2

#define UID_STATE_TOTAL_CURR	2
#define UID_STATE_TOTAL_LAST	3
#define UID_STATE_DEAD_TASKS	4
#define UID_STATE_SIZE		5

struct uid_entry {
	uid_t uid;
	cputime_t utime;
	cputime_t stime;
	cputime_t active_utime;
	cputime_t active_stime;
	unsigned long long active_power;
	unsigned long long power;
	int state;
	struct io_stats io[UID_STATE_SIZE];
	struct hlist_node hash;
};

static struct uid_entry *find_uid_entry(uid_t uid)
{
	struct uid_entry *uid_entry;
	hash_for_each_possible(hash_table, uid_entry, hash, uid) {
		if (uid_entry->uid == uid)
			return uid_entry;
	}
	return NULL;
}

static struct uid_entry *find_or_register_uid(uid_t uid)
{
	struct uid_entry *uid_entry;

	uid_entry = find_uid_entry(uid);
	if (uid_entry)
		return uid_entry;

	uid_entry = kzalloc(sizeof(struct uid_entry), GFP_ATOMIC);
	if (!uid_entry)
		return NULL;

	uid_entry->uid = uid;

	hash_add(hash_table, &uid_entry->hash, uid);

	return uid_entry;
}

static int uid_cputime_show(struct seq_file *m, void *v)
{
	struct uid_entry *uid_entry;
	struct task_struct *task, *temp;
	struct user_namespace *user_ns = current_user_ns();
	cputime_t utime;
	cputime_t stime;
	unsigned long bkt;
	uid_t uid;

	rt_mutex_lock(&uid_lock);

	hash_for_each(hash_table, bkt, uid_entry, hash) {
		uid_entry->active_stime = 0;
		uid_entry->active_utime = 0;
		uid_entry->active_power = 0;
	}

	read_lock(&tasklist_lock);
	do_each_thread(temp, task) {
		uid = from_kuid_munged(user_ns, task_uid(task));
		uid_entry = find_or_register_uid(uid);
		if (!uid_entry) {
			read_unlock(&tasklist_lock);
			rt_mutex_unlock(&uid_lock);
			pr_err("%s: failed to find the uid_entry for uid %d\n",
				__func__, uid);
			return -ENOMEM;
		}
		/* if this task is exiting, we have already accounted for the
		 * time and power.
		 */
		if (task->cpu_power == ULLONG_MAX)
			continue;
		task_cputime_adjusted(task, &utime, &stime);
		uid_entry->active_utime += utime;
		uid_entry->active_stime += stime;
		uid_entry->active_power += task->cpu_power;
	} while_each_thread(temp, task);
	read_unlock(&tasklist_lock);

	hash_for_each(hash_table, bkt, uid_entry, hash) {
		cputime_t total_utime = uid_entry->utime +
							uid_entry->active_utime;
		cputime_t total_stime = uid_entry->stime +
							uid_entry->active_stime;
		unsigned long long total_power = uid_entry->power +
							uid_entry->active_power;
		seq_printf(m, "%d: %llu %llu %llu\n", uid_entry->uid,
			(unsigned long long)jiffies_to_msecs(
				cputime_to_jiffies(total_utime)) * USEC_PER_MSEC,
			(unsigned long long)jiffies_to_msecs(
				cputime_to_jiffies(total_stime)) * USEC_PER_MSEC,
			total_power);
	}

	rt_mutex_unlock(&uid_lock);
	return 0;
}

static int uid_cputime_open(struct inode *inode, struct file *file)
{
	return single_open(file, uid_cputime_show, PDE_DATA(inode));
}

static const struct file_operations uid_cputime_fops = {
	.open		= uid_cputime_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int uid_remove_open(struct inode *inode, struct file *file)
{
	return single_open(file, NULL, NULL);
}

static ssize_t uid_remove_write(struct file *file,
			const char __user *buffer, size_t count, loff_t *ppos)
{
	struct uid_entry *uid_entry;
	struct hlist_node *tmp;
	char uids[128];
	char *start_uid, *end_uid = NULL;
	long int start = 0, end = 0;
	uid_t uid_start, uid_end;

	if (count >= sizeof(uids))
		count = sizeof(uids) - 1;

	if (copy_from_user(uids, buffer, count))
		return -EFAULT;

	uids[count] = '\0';
	end_uid = uids;
	start_uid = strsep(&end_uid, "-");

	if (!start_uid || !end_uid)
		return -EINVAL;

	if (kstrtol(start_uid, 10, &start) != 0 ||
		kstrtol(end_uid, 10, &end) != 0) {
		return -EINVAL;
	}

#define UID_T_MAX (((uid_t)~0U)-1)
	if ((start < 0) || (end < 0) ||
		(start > UID_T_MAX) || (end > UID_T_MAX)) {
		return -EINVAL;
	}

	uid_start = start;
	uid_end = end;

	/* TODO need to unify uid_sys_stats interface with uid_time_in_state.
	 * Here we are reusing remove_uid_range to reduce the number of
	 * sys calls made by userspace clients, remove_uid_range removes uids
	 * from both here as well as from cpufreq uid_time_in_state
	 */
	cpufreq_task_stats_remove_uids(uid_start, uid_end);

	rt_mutex_lock(&uid_lock);

	for (; uid_start <= uid_end; uid_start++) {
		hash_for_each_possible_safe(hash_table, uid_entry, tmp,
							hash, uid_start) {
			if (uid_start == uid_entry->uid) {
				hash_del(&uid_entry->hash);
				kfree(uid_entry);
			}
		}
	}

	rt_mutex_unlock(&uid_lock);

	return count;
}

static const struct file_operations uid_remove_fops = {
	.open		= uid_remove_open,
	.release	= single_release,
	.write		= uid_remove_write,
};

static u64 compute_write_bytes(struct task_struct *task)
{
	if (task->ioac.write_bytes <= task->ioac.cancelled_write_bytes)
		return 0;

	return task->ioac.write_bytes - task->ioac.cancelled_write_bytes;
}

static void add_uid_io_stats(struct uid_entry *uid_entry,
			struct task_struct *task, int slot)
{
	struct io_stats *io_slot = &uid_entry->io[slot];

	io_slot->read_bytes += task->ioac.read_bytes;
	io_slot->write_bytes += compute_write_bytes(task);
	io_slot->rchar += task->ioac.rchar;
	io_slot->wchar += task->ioac.wchar;
	io_slot->fsync += task->ioac.syscfs;
}

static void compute_uid_io_bucket_stats(struct io_stats *io_bucket,
					struct io_stats *io_curr,
					struct io_stats *io_last,
					struct io_stats *io_dead)
{
	s64 delta;

	delta = io_curr->read_bytes + io_dead->read_bytes -
		io_last->read_bytes;
	if (delta > 0)
		io_bucket->read_bytes += delta;

	delta = io_curr->write_bytes + io_dead->write_bytes -
		io_last->write_bytes;
	if (delta > 0)
		io_bucket->write_bytes += delta;

	delta = io_curr->rchar + io_dead->rchar - io_last->rchar;
	if (delta > 0)
		io_bucket->rchar += delta;

	delta = io_curr->wchar + io_dead->wchar - io_last->wchar;
	if (delta > 0)
		io_bucket->wchar += delta;

	delta = io_curr->fsync + io_dead->fsync - io_last->fsync;
	if (delta > 0)
		io_bucket->fsync += delta;

	io_last->read_bytes = io_curr->read_bytes;
	io_last->write_bytes = io_curr->write_bytes;
	io_last->rchar = io_curr->rchar;
	io_last->wchar = io_curr->wchar;
	io_last->fsync = io_curr->fsync;

	memset(io_dead, 0, sizeof(struct io_stats));
}

static void update_io_stats_all_locked(void)
{
	struct uid_entry *uid_entry;
	struct task_struct *task, *temp;
	struct user_namespace *user_ns = current_user_ns();
	unsigned long bkt;
	uid_t uid;

	hash_for_each(hash_table, bkt, uid_entry, hash)
		memset(&uid_entry->io[UID_STATE_TOTAL_CURR], 0,
			sizeof(struct io_stats));

	rcu_read_lock();
	do_each_thread(temp, task) {
		uid = from_kuid_munged(user_ns, task_uid(task));
		uid_entry = find_or_register_uid(uid);
		if (!uid_entry)
			continue;
		add_uid_io_stats(uid_entry, task, UID_STATE_TOTAL_CURR);
	} while_each_thread(temp, task);
	rcu_read_unlock();

	hash_for_each(hash_table, bkt, uid_entry, hash) {
		compute_uid_io_bucket_stats(&uid_entry->io[uid_entry->state],
					&uid_entry->io[UID_STATE_TOTAL_CURR],
					&uid_entry->io[UID_STATE_TOTAL_LAST],
					&uid_entry->io[UID_STATE_DEAD_TASKS]);
	}
}

static void update_io_stats_uid_locked(struct uid_entry *uid_entry)
{
	struct task_struct *task, *temp;
	struct user_namespace *user_ns = current_user_ns();

	memset(&uid_entry->io[UID_STATE_TOTAL_CURR], 0,
		sizeof(struct io_stats));

	rcu_read_lock();
	do_each_thread(temp, task) {
		if (from_kuid_munged(user_ns, task_uid(task)) != uid_entry->uid)
			continue;
		add_uid_io_stats(uid_entry, task, UID_STATE_TOTAL_CURR);
	} while_each_thread(temp, task);
	rcu_read_unlock();

	compute_uid_io_bucket_stats(&uid_entry->io[uid_entry->state],
				&uid_entry->io[UID_STATE_TOTAL_CURR],
				&uid_entry->io[UID_STATE_TOTAL_LAST],
				&uid_entry->io[UID_STATE_DEAD_TASKS]);
}

static int uid_io_show(struct seq_file *m, void *v)
{
	struct uid_entry *uid_entry;
	unsigned long bkt;

	rt_mutex_lock(&uid_lock);

	update_io_stats_all_locked();

	hash_for_each(hash_table, bkt, uid_entry, hash) {
		seq_printf(m, "%d %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu\n",
			uid_entry->uid,
			uid_entry->io[UID_STATE_FOREGROUND].rchar,
			uid_entry->io[UID_STATE_FOREGROUND].wchar,
			uid_entry->io[UID_STATE_FOREGROUND].read_bytes,
			uid_entry->io[UID_STATE_FOREGROUND].write_bytes,
			uid_entry->io[UID_STATE_BACKGROUND].rchar,
			uid_entry->io[UID_STATE_BACKGROUND].wchar,
			uid_entry->io[UID_STATE_BACKGROUND].read_bytes,
			uid_entry->io[UID_STATE_BACKGROUND].write_bytes,
			uid_entry->io[UID_STATE_FOREGROUND].fsync,
			uid_entry->io[UID_STATE_BACKGROUND].fsync);
	}

	rt_mutex_unlock(&uid_lock);

	return 0;
}

static int uid_io_open(struct inode *inode, struct file *file)
{
	return single_open(file, uid_io_show, PDE_DATA(inode));
}

static const struct file_operations uid_io_fops = {
	.open		= uid_io_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int uid_procstat_open(struct inode *inode, struct file *file)
{
	return single_open(file, NULL, NULL);
}

static ssize_t uid_procstat_write(struct file *file,
			const char __user *buffer, size_t count, loff_t *ppos)
{
	struct uid_entry *uid_entry;
	uid_t uid;
	int argc, state;
	char input[128];

	if (count >= sizeof(input))
		return -EINVAL;

	if (copy_from_user(input, buffer, count))
		return -EFAULT;

	input[count] = '\0';

	argc = sscanf(input, "%u %d", &uid, &state);
	if (argc != 2)
		return -EINVAL;

	if (state != UID_STATE_BACKGROUND && state != UID_STATE_FOREGROUND)
		return -EINVAL;

	rt_mutex_lock(&uid_lock);

	uid_entry = find_or_register_uid(uid);
	if (!uid_entry) {
		rt_mutex_unlock(&uid_lock);
		return -EINVAL;
	}

	if (uid_entry->state == state) {
		rt_mutex_unlock(&uid_lock);
		return count;
	}

	update_io_stats_uid_locked(uid_entry);

	uid_entry->state = state;

	rt_mutex_unlock(&uid_lock);

	return count;
}

static const struct file_operations uid_procstat_fops = {
	.open		= uid_procstat_open,
	.release	= single_release,
	.write		= uid_procstat_write,
};

static int process_notifier(struct notifier_block *self,
			unsigned long cmd, void *v)
{
	struct task_struct *task = v;
	struct uid_entry *uid_entry;
	cputime_t utime, stime;
	uid_t uid;

	if (!task)
		return NOTIFY_OK;

	rt_mutex_lock(&uid_lock);
	uid = from_kuid_munged(current_user_ns(), task_uid(task));
	uid_entry = find_or_register_uid(uid);
	if (!uid_entry) {
		pr_err("%s: failed to find uid %d\n", __func__, uid);
		goto exit;
	}

	task_cputime_adjusted(task, &utime, &stime);
	uid_entry->utime += utime;
	uid_entry->stime += stime;
	uid_entry->power += task->cpu_power;
	task->cpu_power = ULLONG_MAX;

	add_uid_io_stats(uid_entry, task, UID_STATE_DEAD_TASKS);

exit:
	rt_mutex_unlock(&uid_lock);
	return NOTIFY_OK;
}

static struct notifier_block process_notifier_block = {
	.notifier_call	= process_notifier,
};

static int __init proc_uid_sys_stats_init(void)
{
	hash_init(hash_table);

	cpu_parent = proc_mkdir("uid_cputime", NULL);
	if (!cpu_parent) {
		pr_err("%s: failed to create uid_cputime proc entry\n",
			__func__);
		goto err;
	}

	proc_create_data("remove_uid_range", 0222, cpu_parent,
		&uid_remove_fops, NULL);
	proc_create_data("show_uid_stat", 0444, cpu_parent,
		&uid_cputime_fops, NULL);

	io_parent = proc_mkdir("uid_io", NULL);
	if (!io_parent) {
		pr_err("%s: failed to create uid_io proc entry\n",
			__func__);
		goto err;
	}

	proc_create_data("stats", 0444, io_parent,
		&uid_io_fops, NULL);

	proc_parent = proc_mkdir("uid_procstat", NULL);
	if (!proc_parent) {
		pr_err("%s: failed to create uid_procstat proc entry\n",
			__func__);
		goto err;
	}

	proc_create_data("set", 0222, proc_parent,
		&uid_procstat_fops, NULL);

	profile_event_register(PROFILE_TASK_EXIT, &process_notifier_block);

	return 0;

err:
	remove_proc_subtree("uid_cputime", NULL);
	remove_proc_subtree("uid_io", NULL);
	remove_proc_subtree("uid_procstat", NULL);
	return -ENOMEM;
}

early_initcall(proc_uid_sys_stats_init);
