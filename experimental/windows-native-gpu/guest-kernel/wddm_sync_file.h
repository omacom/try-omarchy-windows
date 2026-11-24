/* SPDX-License-Identifier: GPL-2.0-only */
/* Included after aperture leases/lock. The native fence page is never read
 * after revocation: that decision and the read share aperture_lock. Polling is
 * a bounded integration baseline, not an interrupt/performance implementation. */
#include <linux/dma-fence.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/sync_file.h>
#include <linux/workqueue.h>

#define WDDM_SYNC_MAX 256
#define WDDM_SYNC_TIMEOUT_MS 5000
static bool enable_native_sync_files;
module_param(enable_native_sync_files, bool, 0400);
MODULE_PARM_DESC(enable_native_sync_files, "Explicit experimental native GPU sync_file export");
static struct workqueue_struct *native_sync_wq;
static u32 native_sync_held, native_sync_pending;
static u64 native_sync_created, native_sync_signaled, native_sync_errors, native_sync_released;
struct native_sync {
	struct dma_fence fence;
	spinlock_t lock;
	struct delayed_work poll;
	struct work_struct free;
	struct rcu_head reclaim;
	struct file *source;
	struct file *native_source;
	void __iomem *word;
	u64 target;
	unsigned long deadline;
	bool published;
};
static const char *native_sync_driver(struct dma_fence *fence) { return "wddm-aperture"; }
static const char *native_sync_timeline(struct dma_fence *fence) { return "native-monitored-fence"; }
static bool native_sync_enable(struct dma_fence *fence) { return true; }
static void native_sync_free(struct work_struct *work)
{
	struct native_sync *sync = container_of(work, struct native_sync, free);
	iounmap(sync->word);
	fput(sync->source);
	if (sync->native_source) fput(sync->native_source);
	mutex_lock(&aperture_lock);
	--native_sync_held;
	if (sync->published) ++native_sync_released;
	mutex_unlock(&aperture_lock);
	kfree(sync);
	/* Module exit destroys/flushed this queue before its code is removed. */
	module_put(THIS_MODULE);
}
static void native_sync_rcu_release(struct rcu_head *head)
{
	struct native_sync *sync = container_of(head, struct native_sync, reclaim);
	queue_work(native_sync_wq, &sync->free);
}
static void native_sync_release(struct dma_fence *fence)
{
	struct native_sync *sync = container_of(fence, struct native_sync, fence);
	/* Last put may run in atomic context. Fence memory/ops obey the RCU
	 * lifetime contract; iounmap/fput then run in process context. The module
	 * reference survives the grace period and the final cleanup worker. */
	call_rcu(&sync->reclaim, native_sync_rcu_release);
}
static const struct dma_fence_ops native_sync_ops = {
	.get_driver_name = native_sync_driver, .get_timeline_name = native_sync_timeline,
	.enable_signaling = native_sync_enable, .release = native_sync_release,
};
static void native_sync_poll(struct work_struct *work)
{
	struct native_sync *sync = container_of(to_delayed_work(work), struct native_sync, poll);
	struct aperture_lease *lease = sync->source->private_data;
	int status = 0;
	u64 value = 0;
	mutex_lock(&aperture_lock);
	if (!lease->valid || !fence_hub.pci || !cpu_hub.pci) status = -ENODEV;
	else {
		value = readq(sync->word);
		if (value == U64_MAX) status = -EIO;
		else if (value >= sync->target) status = 1;
		else if (time_after_eq(jiffies, sync->deadline)) status = -ETIMEDOUT;
	}
	if (status) {
		--native_sync_pending;
		if (status > 0) ++native_sync_signaled; else ++native_sync_errors;
	}
	mutex_unlock(&aperture_lock);
	if (!status) {
		queue_delayed_work(native_sync_wq, &sync->poll, max_t(unsigned long, 1, msecs_to_jiffies(1)));
		return;
	}
	if (status < 0) dma_fence_set_error(&sync->fence, status);
	dma_fence_signal(&sync->fence);
	dma_fence_put(&sync->fence); /* Retained polling reference. */
}
/* Prepare an owned read-only monitored-fence view without publishing work.
 * Callers can roll back an entire multi-fence registration. The returned
 * initial reference owns the mapping and is released with dma_fence_put(). */
static struct native_sync *native_sync_prepare(struct file *source, u64 offset, u64 value)
{
	struct aperture_lease *lease = source->private_data;
	struct native_sync *sync;
	int result = 0;
	if ((offset & 7) || value == U64_MAX) return ERR_PTR(-EINVAL);
	sync = kzalloc(sizeof(*sync), GFP_KERNEL);
	if (!sync) return ERR_PTR(-ENOMEM);
	mutex_lock(&aperture_lock);
	if (!lease->valid || !fence_hub.pci || !cpu_hub.pci) result = -ENODEV;
	else if (lease->kind != WDDM_APERTURE_FENCE || lease->writable ||
		 offset >= lease->bytes || sizeof(u64) > lease->bytes - offset) result = -EINVAL;
	else if (native_sync_held >= WDDM_SYNC_MAX) result = -ENOSPC;
	else {
		sync->word = ioremap(lease->physical + offset, sizeof(u64));
		if (!sync->word) result = -ENOMEM;
		else { sync->source = get_file(source); ++native_sync_held; }
	}
	mutex_unlock(&aperture_lock);
	if (result) { kfree(sync); return ERR_PTR(result); }
	__module_get(THIS_MODULE);
	spin_lock_init(&sync->lock);
	sync->target = value;
	INIT_WORK(&sync->free, native_sync_free);
	INIT_DELAYED_WORK(&sync->poll, native_sync_poll);
	dma_fence_init(&sync->fence, &native_sync_ops, &sync->lock, dma_fence_context_alloc(1), 1);
	return sync;
}
static long native_sync_create(struct file *control, void __user *pointer, bool linked)
{
	struct wddm_sync_file_create arg;
	struct wddm_sync_file_native_create native_arg;
	struct file *native_source = NULL;
	struct aperture_lease *lease;
	struct native_sync *sync;
	struct sync_file *file;
	int fd, result = -ENOENT;
	if (!capable(CAP_SYS_RAWIO)) return -EPERM;
	if (!enable_native_sync_files) return -EOPNOTSUPP;
	if (linked) {
		if (copy_from_user(&native_arg, pointer, sizeof(native_arg))) return -EFAULT;
		arg = native_arg.sync;
		if (native_arg.reserved || native_arg.native_fd < 0) return -EINVAL;
	} else if (copy_from_user(&arg, pointer, sizeof(arg))) return -EFAULT;
	if (arg.version != WDDM_APERTURE_VERSION || arg.flags || !arg.cookie ||
	    (!linked && !arg.value) || arg.value == U64_MAX || arg.fd || arg.reserved || (arg.offset & 7)) return -EINVAL;
	if (linked) {
		struct wddm_object *object;
		native_source = fget(native_arg.native_fd);
		if (!native_source) return -EBADF;
		if (native_source->f_op != &object_fops) { fput(native_source); return -EINVAL; }
		object = native_source->private_data;
		mutex_lock(&object_lock);
		result = !object->valid ? -ENODEV : object->kind != WDDM_OBJECT_SYNC ? -EINVAL : 0;
		mutex_unlock(&object_lock);
		if (result) { fput(native_source); return result; }
	}
	sync = kzalloc(sizeof(*sync), GFP_KERNEL);
	if (!sync) { if (native_source) fput(native_source); return -ENOMEM; }
	sync->native_source = native_source;
	result = -ENOENT;
	mutex_lock(&aperture_lock);
	list_for_each_entry(lease, &aperture_leases, link) {
		if (lease->cookie != arg.cookie || lease->owner != control->private_data) continue;
		if (!lease->valid || !fence_hub.pci || !cpu_hub.pci) { result = -ENODEV; break; }
		if (lease->kind != WDDM_APERTURE_FENCE || lease->writable ||
		    arg.offset >= lease->bytes || sizeof(u64) > lease->bytes - arg.offset) { result = -EINVAL; break; }
		if (native_sync_held >= WDDM_SYNC_MAX) { result = -ENOSPC; break; }
		sync->word = ioremap(lease->physical + arg.offset, sizeof(u64));
		if (!sync->word) { result = -ENOMEM; break; }
		sync->source = get_file(lease->file);
		++native_sync_held; result = 0; break;
	}
	mutex_unlock(&aperture_lock);
	if (result) { if (native_source) fput(native_source); kfree(sync); return result; }
	__module_get(THIS_MODULE);
	spin_lock_init(&sync->lock);
	sync->target = arg.value;
	INIT_WORK(&sync->free, native_sync_free);
	INIT_DELAYED_WORK(&sync->poll, native_sync_poll);
	/* Independent one-shot context; the native 64-bit target is kept above.
	 * No ordering comparison truncates that value into dma_fence's seqno. */
	dma_fence_init(&sync->fence, &native_sync_ops, &sync->lock, dma_fence_context_alloc(1), 1);
	file = sync_file_create(&sync->fence);
	if (!file) { dma_fence_put(&sync->fence); return -ENOMEM; }
	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) { fput(file->file); dma_fence_put(&sync->fence); return fd; }
	arg.fd = fd;
	if (linked) native_arg.sync = arg;
	if (copy_to_user(pointer, linked ? (void *)&native_arg : (void *)&arg,
			 linked ? sizeof(native_arg) : sizeof(arg))) {
		put_unused_fd(fd); fput(file->file); dma_fence_put(&sync->fence); return -EFAULT;
	}
	mutex_lock(&aperture_lock);
	sync->published = true;
	++native_sync_created; ++native_sync_pending;
	mutex_unlock(&aperture_lock);
	sync->deadline = jiffies + msecs_to_jiffies(WDDM_SYNC_TIMEOUT_MS);
	/* Initial reference belongs to polling until signaled/error; the sync_file
 * holds a separate reference. Last-FD close cannot free a pending worker. */
	queue_delayed_work(native_sync_wq, &sync->poll, 0);
	fd_install(fd, file->file);
	return 0;
}
static long native_sync_resolve(u64 owner, void __user *pointer)
{
	struct wddm_sync_file_native_resolve arg;
	struct dma_fence *fence;
	struct native_sync *sync;
	struct wddm_object *object;
	struct file *source = NULL;
	int fd, result = 0;
	if (!capable(CAP_SYS_RAWIO)) return -EPERM;
	if (!enable_native_sync_files) return -EOPNOTSUPP;
	if (copy_from_user(&arg, pointer, sizeof(arg))) return -EFAULT;
	if (arg.version != WDDM_APERTURE_VERSION || arg.flags || arg.fd < 0 ||
	    arg.native_fd || arg.value || arg.reserved) return -EINVAL;
	fence = sync_file_get_fence(arg.fd);
	if (!fence) return -EINVAL;
	/* Our custom release prevents driver detachment. A retained fence reference
	 * and the ops identity protect container_of even after signaling. Foreign
	 * sync_files, fence arrays/chains and unlinked exports are rejected. */
	if (rcu_access_pointer(fence->ops) != &native_sync_ops) { result = -EINVAL; goto put; }
	sync = container_of(fence, struct native_sync, fence);
	result = dma_fence_get_status(fence);
	if (result < 0) goto put;
	if (!result) {
		/* The polling worker may not yet have recorded a lease revocation.
		 * Do not turn a canceled Linux dependency into an unbounded GPU wait.
		 * Successful completed fences can outlive their original CPU lease. */
		struct aperture_lease *lease = sync->source->private_data;
		mutex_lock(&aperture_lock);
		if (!lease->valid || !fence_hub.pci || !cpu_hub.pci) result = -ENODEV;
		mutex_unlock(&aperture_lock);
		if (result < 0) goto put;
	}
	result = 0;
	if (!sync->native_source) { result = -EOPNOTSUPP; goto put; }
	object = sync->native_source->private_data;
	mutex_lock(&object_lock);
	if (!object->valid) result = -ENODEV;
	else if (object->owner != owner || object->kind != WDDM_OBJECT_SYNC) result = -EPERM;
	else source = get_file(sync->native_source);
	mutex_unlock(&object_lock);
	if (result) goto put;
	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) { result = fd; fput(source); goto put; }
	arg.native_fd = fd; arg.value = sync->target;
	if (copy_to_user(pointer, &arg, sizeof(arg))) {
		put_unused_fd(fd); fput(source); result = -EFAULT; goto put;
	}
	fd_install(fd, source);
put:
	dma_fence_put(fence);
	return result;
}
static long native_sync_stats(void __user *pointer)
{
	struct wddm_sync_file_stats arg;
	if (copy_from_user(&arg, pointer, sizeof(arg))) return -EFAULT;
	if (arg.version != WDDM_APERTURE_VERSION) return -EINVAL;
	mutex_lock(&aperture_lock);
	arg = (struct wddm_sync_file_stats){ WDDM_APERTURE_VERSION, native_sync_held, native_sync_pending, 0,
		native_sync_created, native_sync_signaled, native_sync_errors, native_sync_released };
	mutex_unlock(&aperture_lock);
	return copy_to_user(pointer, &arg, sizeof(arg)) ? -EFAULT : 0;
}
