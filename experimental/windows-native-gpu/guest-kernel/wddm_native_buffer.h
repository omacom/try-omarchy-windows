/* SPDX-License-Identifier: GPL-2.0-only */
/* Genuine DMA-BUF ownership of an opaque native capability. No guest PFNs,
 * CPU access, foreign-device DMA mapping or implicit synchronization yet. */
#include <linux/dma-buf.h>
#include "wddm_native_prime.h"

static bool enable_native_buffers;
module_param(enable_native_buffers, bool, 0444);
MODULE_PARM_DESC(enable_native_buffers, "Enable experimental owned opaque native-resource DMA-BUF files");

struct wddm_native_buffer {
	struct file *source;
	u64 bytes;
	bool published;
};
static DEFINE_MUTEX(native_buffer_lock);
static u32 native_buffers_held;
static u64 native_buffer_bytes, native_buffers_created, native_buffers_released;

static int native_buffer_attach(struct dma_buf *buffer, struct dma_buf_attachment *attachment)
{
	return -EOPNOTSUPP;
}
static struct sg_table *native_buffer_map(struct dma_buf_attachment *attachment,
					 enum dma_data_direction direction)
{
	return ERR_PTR(-EOPNOTSUPP);
}
static void native_buffer_unmap(struct dma_buf_attachment *attachment,
			       struct sg_table *table, enum dma_data_direction direction)
{
}
static int native_buffer_cpu_access(struct dma_buf *buffer, enum dma_data_direction direction)
{
	return -EOPNOTSUPP;
}
static int native_buffer_mmap(struct dma_buf *buffer, struct vm_area_struct *vma)
{
	return -ENODEV;
}
static int native_buffer_vmap(struct dma_buf *buffer, struct iosys_map *map)
{
	return -EOPNOTSUPP;
}
static void native_buffer_release(struct dma_buf *buffer)
{
	struct wddm_native_buffer *native = buffer->priv;
	fput(native->source);
	mutex_lock(&native_buffer_lock);
	--native_buffers_held; native_buffer_bytes -= native->bytes;
	if (native->published) ++native_buffers_released;
	mutex_unlock(&native_buffer_lock);
	kfree(native);
}
static const struct dma_buf_ops native_buffer_ops = {
	.attach = native_buffer_attach, .map_dma_buf = native_buffer_map,
	.unmap_dma_buf = native_buffer_unmap, .begin_cpu_access = native_buffer_cpu_access,
	.end_cpu_access = native_buffer_cpu_access, .mmap = native_buffer_mmap,
	.vmap = native_buffer_vmap, .release = native_buffer_release,
};

int wddm_native_buffer_size(struct dma_buf *buffer, u64 *bytes)
{
	struct wddm_native_buffer *native;
	struct wddm_object *object;
	int result = 0;
	if (!enable_native_buffers || !buffer || !bytes || buffer->ops != &native_buffer_ops)
		return -EINVAL;
	native = buffer->priv;
	object = native->source->private_data;
	mutex_lock(&object_lock);
	if (!object->valid || object->kind != WDDM_OBJECT_RESOURCE) result = -ENODEV;
	else if (!native->published || !native->bytes || native->bytes > WDDM_NATIVE_BUFFER_MAX_BYTES || buffer->size != native->bytes) result = -EINVAL;
	else *bytes = native->bytes;
	mutex_unlock(&object_lock);
	return result;
}
EXPORT_SYMBOL_GPL(wddm_native_buffer_size);

static struct file *object_fd_get(int fd)
{
	struct file *file = fget(fd);
	struct dma_buf *buffer;
	struct wddm_native_buffer *native;
	if (!file) return ERR_PTR(-EBADF);
	if (file->f_op == &object_fops) return file;
	fput(file);
	if (!enable_native_buffers) return ERR_PTR(-EINVAL);
	buffer = dma_buf_get(fd);
	if (IS_ERR(buffer)) return ERR_CAST(buffer);
	if (buffer->ops != &native_buffer_ops) {
		dma_buf_put(buffer); return ERR_PTR(-EINVAL);
	}
	native = buffer->priv;
	file = get_file(native->source);
	dma_buf_put(buffer);
	return file;
}

static long native_buffer_create(u64 owner, void __user *pointer)
{
	struct wddm_native_buffer_create arg;
	struct wddm_native_buffer *native;
	struct wddm_object *object;
	struct dma_buf *buffer;
	struct file *source;
	DEFINE_DMA_BUF_EXPORT_INFO(info);
	int fd, result = 0;
	if (!enable_native_buffers) return -EOPNOTSUPP;
	if (!capable(CAP_SYS_RAWIO)) return -EPERM;
	if (copy_from_user(&arg, pointer, sizeof(arg))) return -EFAULT;
	if (arg.version != WDDM_APERTURE_VERSION || arg.flags || arg.native_fd < 0 || arg.fd ||
	    !arg.bytes || arg.bytes > WDDM_NATIVE_BUFFER_MAX_BYTES || arg.reserved) return -EINVAL;
	/* Never wrap another DMA-BUF, a fence or a user-authored ticket. */
	source = fget(arg.native_fd);
	if (!source) return -EBADF;
	if (source->f_op != &object_fops) { fput(source); return -EINVAL; }
	object = source->private_data;
	mutex_lock(&object_lock);
	if (!object->valid) result = -ENODEV;
	else if (object->owner != owner || object->kind != WDDM_OBJECT_RESOURCE) result = -EPERM;
	mutex_unlock(&object_lock);
	if (result) { fput(source); return result; }
	native = kzalloc(sizeof(*native), GFP_KERNEL);
	if (!native) { fput(source); return -ENOMEM; }
	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) { fput(source); kfree(native); return fd; }
	native->source = source; native->bytes = arg.bytes;
	mutex_lock(&native_buffer_lock);
	if (native_buffers_held >= WDDM_NATIVE_BUFFER_MAX ||
	    native_buffer_bytes > WDDM_NATIVE_BUFFER_TOTAL_BYTES - arg.bytes) result = -ENOSPC;
	else { ++native_buffers_held; native_buffer_bytes += arg.bytes; }
	mutex_unlock(&native_buffer_lock);
	if (result) { put_unused_fd(fd); fput(source); kfree(native); return result; }
	info.exp_name = "wddm-native"; info.owner = THIS_MODULE;
	info.ops = &native_buffer_ops; info.size = arg.bytes; info.flags = O_RDWR;
	info.priv = native;
	buffer = dma_buf_export(&info);
	if (IS_ERR(buffer)) {
		mutex_lock(&native_buffer_lock);
		--native_buffers_held; native_buffer_bytes -= arg.bytes;
		mutex_unlock(&native_buffer_lock);
		put_unused_fd(fd); fput(source); kfree(native); return PTR_ERR(buffer);
	}
	arg.fd = fd;
	if (copy_to_user(pointer, &arg, sizeof(arg))) {
		put_unused_fd(fd); dma_buf_put(buffer); return -EFAULT;
	}
	mutex_lock(&native_buffer_lock);
	native->published = true; ++native_buffers_created;
	mutex_unlock(&native_buffer_lock);
	fd_install(fd, buffer->file);
	return 0;
}

static long native_buffer_info(u64 owner, void __user *pointer)
{
	struct wddm_native_buffer_info arg;
	struct wddm_native_buffer *native;
	struct wddm_object *object;
	struct dma_buf *buffer;
	int result = 0;
	if (!enable_native_buffers) return -EOPNOTSUPP;
	if (!capable(CAP_SYS_RAWIO)) return -EPERM;
	if (copy_from_user(&arg, pointer, sizeof(arg))) return -EFAULT;
	if (arg.version != WDDM_APERTURE_VERSION || arg.flags || arg.fd < 0 || arg.bytes || arg.reserved) return -EINVAL;
	buffer = dma_buf_get(arg.fd);
	if (IS_ERR(buffer)) return PTR_ERR(buffer);
	if (buffer->ops != &native_buffer_ops) { dma_buf_put(buffer); return -EINVAL; }
	native = buffer->priv; object = native->source->private_data;
	mutex_lock(&object_lock);
	if (!object->valid) result = -ENODEV;
	else if (object->owner != owner || object->kind != WDDM_OBJECT_RESOURCE) result = -EPERM;
	else arg.bytes = native->bytes;
	mutex_unlock(&object_lock);
	if (!result && copy_to_user(pointer, &arg, sizeof(arg))) result = -EFAULT;
	dma_buf_put(buffer);
	return result;
}

static long native_buffer_stats(void __user *pointer)
{
	struct wddm_native_buffer_stats arg;
	if (copy_from_user(&arg, pointer, sizeof(arg))) return -EFAULT;
	if (arg.version != WDDM_APERTURE_VERSION) return -EINVAL;
	mutex_lock(&native_buffer_lock);
	arg = (struct wddm_native_buffer_stats){ WDDM_APERTURE_VERSION, native_buffers_held,
		native_buffer_bytes, native_buffers_created, native_buffers_released };
	mutex_unlock(&native_buffer_lock);
	return copy_to_user(pointer, &arg, sizeof(arg)) ? -EFAULT : 0;
}
static long native_buffer_caps(void __user *pointer)
{
	struct wddm_native_buffer_caps arg;
	if (copy_from_user(&arg, pointer, sizeof(arg))) return -EFAULT;
	if (arg.version != WDDM_APERTURE_VERSION || arg.flags || arg.reserved[0] || arg.reserved[1]) return -EINVAL;
	arg.flags = enable_native_buffers ? WDDM_NATIVE_BUFFER_ENABLED : 0;
	return copy_to_user(pointer, &arg, sizeof(arg)) ? -EFAULT : 0;
}
