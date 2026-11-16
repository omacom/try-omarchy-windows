/* SPDX-License-Identifier: GPL-2.0-only */
/* Restricted lease capabilities, real read-only GPU fence observations and
 * in-kernel eventfd notification. Never CPU-signal a GPU fence or block the
 * registration ioctl. One bounded work item supports both ANY and ALL waits. */
#include <linux/eventfd.h>

static atomic64_t event_wait_held, event_wait_pending, event_wait_created;
static atomic64_t event_wait_notified, event_wait_errors, event_wait_released;
struct native_event_wait {
	struct native_sync **views;
	u32 count, flags;
	struct eventfd_ctx *event;
	struct delayed_work poll;
};
static void native_event_free_views(struct native_event_wait *wait)
{
	u32 index;
	if (wait->views) {
		for (index = 0; index < wait->count; ++index)
			if (wait->views[index]) dma_fence_put(&wait->views[index]->fence);
		kfree(wait->views);
	}
	if (wait->event) eventfd_ctx_put(wait->event);
}
static void native_event_poll(struct work_struct *work)
{
	struct native_event_wait *wait = container_of(to_delayed_work(work), struct native_event_wait, poll);
	bool any = false, all = true;
	u32 index;
	int error = 0;
	mutex_lock(&aperture_lock);
	for (index = 0; index < wait->count; ++index) {
		struct native_sync *view = wait->views[index];
		struct aperture_lease *lease = view->source->private_data;
		u64 observed;
		if (!lease->valid || !fence_hub.pci || !cpu_hub.pci) { error = -ENODEV; break; }
		observed = readq(view->word);
		if (observed == U64_MAX) { error = -EIO; break; }
		any |= observed >= view->target;
		all &= observed >= view->target;
	}
	mutex_unlock(&aperture_lock);
	if (!error && !((wait->flags & WDDM_FENCE_EVENTFD_WAIT_ANY) ? any : all)) {
		/* Duration is unbounded, resources are bounded. A legal future target
		 * must not complete because a timer expired. Lease revocation/process
		 * exit cancels and releases the registration. */
		queue_delayed_work(native_sync_wq, &wait->poll, max_t(unsigned long, 1, msecs_to_jiffies(1)));
		return;
	}
	if (!error) {
		/* Cannot block on a full counter or alter application file flags.
		 * The retained eventfd context pins identity across FD-number reuse. */
		eventfd_signal(wait->event);
		atomic64_inc(&event_wait_notified);
	} else {
		/* Cancellation/device loss is not proof of GPU retirement. */
		atomic64_inc(&event_wait_errors);
		pr_info("WDDM_EVENTFD_WAIT_CANCEL error=%d notified=false\n", error);
	}
	atomic64_dec(&event_wait_pending);
	native_event_free_views(wait);
	atomic64_inc(&event_wait_released);
	atomic64_dec(&event_wait_held);
	kfree(wait);
	/* Module exit flushes this workqueue before removing its code. */
	module_put(THIS_MODULE);
}
static long native_event_stats(void __user *pointer)
{
	struct wddm_fence_eventfd_stats stats;
	if (copy_from_user(&stats, pointer, sizeof(stats))) return -EFAULT;
	if (stats.version != WDDM_APERTURE_VERSION || stats.held || stats.pending || stats.reserved ||
	    stats.created || stats.notified || stats.errors || stats.released) return -EINVAL;
	stats.held = atomic64_read(&event_wait_held);
	stats.pending = atomic64_read(&event_wait_pending);
	stats.created = atomic64_read(&event_wait_created);
	stats.notified = atomic64_read(&event_wait_notified);
	stats.errors = atomic64_read(&event_wait_errors);
	stats.released = atomic64_read(&event_wait_released);
	return copy_to_user(pointer, &stats, sizeof(stats)) ? -EFAULT : 0;
}
static long native_event_register(struct file *authority, void __user *pointer)
{
	struct wddm_fence_eventfd_wait arg;
	struct wddm_fence_eventfd_point *points = NULL;
	struct native_event_wait *wait = NULL;
	struct aperture_lease *owner = authority->private_data;
	u32 index;
	int result = 0;
	if (copy_from_user(&arg, pointer, sizeof(arg))) return -EFAULT;
	if (arg.version != WDDM_APERTURE_VERSION || (arg.flags & ~WDDM_FENCE_EVENTFD_WAIT_ANY) ||
	    !arg.count || arg.count > WDDM_FENCE_EVENTFD_MAX_POINTS || arg.event_fd < 0 ||
	    !arg.points || arg.reserved[0] || arg.reserved[1]) return -EINVAL;
	mutex_lock(&aperture_lock);
	result = !owner->valid ? -ENODEV : owner->kind != WDDM_APERTURE_FENCE || owner->writable ? -EINVAL : 0;
	mutex_unlock(&aperture_lock);
	if (result) return result;
	if (atomic64_inc_return(&event_wait_held) > WDDM_FENCE_EVENTFD_MAX_WAITS) {
		atomic64_dec(&event_wait_held); return -ENOSPC;
	}
	points = memdup_user(u64_to_user_ptr(arg.points), array_size(arg.count, sizeof(*points)));
	if (IS_ERR(points)) { result = PTR_ERR(points); points = NULL; goto out; }
	wait = kzalloc(sizeof(*wait), GFP_KERNEL);
	if (!wait) { result = -ENOMEM; goto out; }
	wait->count = arg.count; wait->flags = arg.flags;
	wait->views = kcalloc(arg.count, sizeof(*wait->views), GFP_KERNEL);
	if (!wait->views) { result = -ENOMEM; goto out; }
	wait->event = eventfd_ctx_fdget(arg.event_fd);
	if (IS_ERR(wait->event)) { result = PTR_ERR(wait->event); wait->event = NULL; goto out; }
	for (index = 0; index < arg.count; ++index) {
		struct file *source;
		struct aperture_lease *lease;
		if (points[index].lease_fd < 0 || points[index].reserved ||
		    (points[index].offset & 7) || points[index].value == U64_MAX) { result = -EINVAL; goto out; }
		source = fget(points[index].lease_fd);
		if (!source) { result = -EBADF; goto out; }
		if (source->f_op != &lease_fops) { fput(source); result = -EINVAL; goto out; }
		lease = source->private_data;
		if (lease->owner != owner->owner) { fput(source); result = -EPERM; goto out; }
		wait->views[index] = native_sync_prepare(source, points[index].offset, points[index].value);
		fput(source);
		if (IS_ERR(wait->views[index])) {
			result = PTR_ERR(wait->views[index]); wait->views[index] = NULL; goto out;
		}
	}
	/* Validate every descriptor, owner, range, value and allocation before
	 * publishing any work. Partial registrations cannot signal an event. */
	INIT_DELAYED_WORK(&wait->poll, native_event_poll);
	__module_get(THIS_MODULE);
	atomic64_inc(&event_wait_created);
	atomic64_inc(&event_wait_pending);
	queue_delayed_work(native_sync_wq, &wait->poll, 0);
	kfree(points);
	return 0;
out:
	if (wait) native_event_free_views(wait);
	kfree(wait); kfree(points);
	atomic64_dec(&event_wait_held);
	return result;
}
static long lease_ioctl(struct file *file, unsigned int command, unsigned long argument)
{
	if (!enable_native_sync_files) return -EOPNOTSUPP;
	if (command == WDDM_FENCE_EVENTFD_WAIT)
		return native_event_register(file, (void __user *)argument);
	if (command == WDDM_FENCE_EVENTFD_STATS)
		return native_event_stats((void __user *)argument);
	return -ENOTTY;
}
static void native_event_exit_summary(void)
{
	if (!atomic64_read(&event_wait_created)) return;
	pr_info("WDDM_EVENTFD_WAIT_EXIT created=%lld notified=%lld errors=%lld released=%lld held=%lld pending=%lld\n",
		atomic64_read(&event_wait_created), atomic64_read(&event_wait_notified),
		atomic64_read(&event_wait_errors), atomic64_read(&event_wait_released),
		atomic64_read(&event_wait_held), atomic64_read(&event_wait_pending));
}
