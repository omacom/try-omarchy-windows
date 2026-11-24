/* SPDX-License-Identifier: GPL-2.0-only */
/* Internal implementation. A trusted broker reaps closed/revoked tickets
 * and closes their native reference exactly once. Controller and anonymous
 * file have separate references; dup/SCM_RIGHTS delay the file's release. */
#include <linux/refcount.h>
struct wddm_object {
	struct list_head link;
	refcount_t refs;
	u64 owner, cookie;
	u32 ticket, kind;
	bool valid, published;
};
/* Defined by the optional DMA-BUF implementation after this include. */
static struct file *object_fd_get(int fd);
static DEFINE_MUTEX(object_lock);
static LIST_HEAD(wddm_objects);
static u64 object_cookie, objects_created, objects_closed, objects_revoked;
static u32 objects_held, objects_active;
static void object_put(struct wddm_object *object)
{
	if (refcount_dec_and_test(&object->refs)) kfree(object);
}
static int object_release(struct inode *inode, struct file *file)
{
	struct wddm_object *object = file->private_data;
	mutex_lock(&object_lock);
	if (object->valid) { object->valid = false; --objects_active; }
	if (object->published) ++objects_closed;
	mutex_unlock(&object_lock);
	object_put(object);
	return 0;
}
static const struct file_operations object_fops = {
	.owner = THIS_MODULE, .release = object_release,
};
static void objects_close_owner(u64 owner)
{
	struct wddm_object *object, *next;
	mutex_lock(&object_lock);
	list_for_each_entry_safe(object, next, &wddm_objects, link) {
		if (object->owner != owner) continue;
		if (object->valid) { object->valid = false; --objects_active; ++objects_revoked; }
		list_del(&object->link); --objects_held; object_put(object);
	}
	mutex_unlock(&object_lock);
}
static long object_create(u64 owner, void __user *pointer)
{
	struct wddm_object_create arg;
	struct wddm_object *object;
	struct file *file;
	int fd, result = 0;
	if (!capable(CAP_SYS_RAWIO)) return -EPERM;
	if (copy_from_user(&arg, pointer, sizeof(arg))) return -EFAULT;
	if (arg.version != WDDM_APERTURE_VERSION || (arg.kind != WDDM_OBJECT_SYNC && arg.kind != WDDM_OBJECT_RESOURCE) ||
	    !arg.ticket || arg.ticket == U32_MAX || arg.cookie || arg.fd || arg.reserved[0] || arg.reserved[1]) return -EINVAL;
	object = kzalloc(sizeof(*object), GFP_KERNEL);
	if (!object) return -ENOMEM;
	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) { kfree(object); return fd; }
	/* No read/mmap. Resolve authenticates exact fops and immutable controller
	 * identity; copied bytes in a memfd can never impersonate this ticket. */
	file = anon_inode_create_getfile("[wddm-object]", &object_fops, object, O_RDONLY, NULL);
	if (IS_ERR(file)) { put_unused_fd(fd); kfree(object); return PTR_ERR(file); }
	refcount_set(&object->refs, 2);
	mutex_lock(&object_lock);
	if (objects_held >= WDDM_OBJECT_MAX || object_cookie == U64_MAX) result = -ENOSPC;
	else {
		struct wddm_object *other;
		list_for_each_entry(other, &wddm_objects, link) {
			if (other->owner == owner && other->ticket == arg.ticket) { result = -EEXIST; break; }
		}
	}
	if (!result) {
		object->owner = owner; object->cookie = ++object_cookie;
		object->kind = arg.kind; object->ticket = arg.ticket; object->valid = true;
		list_add_tail(&object->link, &wddm_objects); ++objects_held; ++objects_active;
		arg.cookie = object->cookie; arg.fd = fd;
		if (copy_to_user(pointer, &arg, sizeof(arg))) {
			object->valid = false; --objects_active; --objects_held; list_del(&object->link); result = -EFAULT;
		} else { object->published = true; ++objects_created; }
	}
	mutex_unlock(&object_lock);
	if (result) { object_put(object); put_unused_fd(fd); fput(file); return result; }
	fd_install(fd, file);
	return 0;
}
static long object_ioctl(u64 owner, unsigned int request, void __user *pointer)
{
	struct wddm_object *object;
	struct file *file;
	int result = 0;
	if (request == WDDM_OBJECT_CREATE) return object_create(owner, pointer);
	if (request == WDDM_OBJECT_RESOLVE) {
		struct wddm_object_resolve arg;
		if (copy_from_user(&arg, pointer, sizeof(arg))) return -EFAULT;
		if (arg.version != WDDM_APERTURE_VERSION || (arg.kind != WDDM_OBJECT_SYNC && arg.kind != WDDM_OBJECT_RESOURCE) || arg.fd < 0 || arg.ticket || arg.cookie || arg.reserved) return -EINVAL;
		file = object_fd_get(arg.fd);
		if (IS_ERR(file)) return PTR_ERR(file);
		object = file->private_data;
		mutex_lock(&object_lock);
		if (!object->valid) result = -ENODEV;
		else if (object->owner != owner || object->kind != arg.kind) result = -EPERM;
		else { arg.ticket = object->ticket; arg.cookie = object->cookie;
			if (copy_to_user(pointer, &arg, sizeof(arg))) result = -EFAULT; }
		mutex_unlock(&object_lock);
		fput(file);
		return result;
	}
	if (request == WDDM_OBJECT_REVOKE) {
		struct wddm_aperture_revoke arg;
		if (copy_from_user(&arg, pointer, sizeof(arg))) return -EFAULT;
		if (arg.version != WDDM_APERTURE_VERSION || arg.reserved || !arg.cookie) return -EINVAL;
		result = -ENOENT;
		mutex_lock(&object_lock);
		list_for_each_entry(object, &wddm_objects, link) {
			if (object->owner != owner || object->cookie != arg.cookie || !object->valid) continue;
			object->valid = false; --objects_active; ++objects_revoked; result = 0; break;
		}
		mutex_unlock(&object_lock);
		return result;
	}
	if (request == WDDM_OBJECT_REAP) {
		struct wddm_object_reap arg;
		if (copy_from_user(&arg, pointer, sizeof(arg))) return -EFAULT;
		if (arg.version != WDDM_APERTURE_VERSION || arg.kind || arg.ticket || arg.reserved || arg.cookie) return -EINVAL;
		result = -EAGAIN;
		mutex_lock(&object_lock);
		list_for_each_entry(object, &wddm_objects, link) {
			if (object->owner != owner || object->valid) continue;
			arg.kind = object->kind; arg.ticket = object->ticket; arg.cookie = object->cookie;
			if (copy_to_user(pointer, &arg, sizeof(arg))) { result = -EFAULT; break; }
			list_del(&object->link); --objects_held; object_put(object); result = 0; break;
		}
		mutex_unlock(&object_lock);
		return result;
	}
	if (request == WDDM_OBJECT_STATS) {
		struct wddm_object_stats arg;
		if (copy_from_user(&arg, pointer, sizeof(arg))) return -EFAULT;
		if (arg.version != WDDM_APERTURE_VERSION) return -EINVAL;
		mutex_lock(&object_lock);
		arg = (struct wddm_object_stats){ WDDM_APERTURE_VERSION, objects_held, objects_active, 0,
			objects_created, objects_closed, objects_revoked };
		mutex_unlock(&object_lock);
		return copy_to_user(pointer, &arg, sizeof(arg)) ? -EFAULT : 0;
	}
	return -ENOTTY;
}
