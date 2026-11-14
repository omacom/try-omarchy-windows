// SPDX-License-Identifier: GPL-2.0-only
// Guest-only render-node entry point for the private D3D12 Gallium backend.
// GPU calls and owned native-buffer FDs still use the authenticated broker.
// Optional PRIME handles retain owned opaque DMA-BUFs; no DMA attachment,
// guest-memory backing, KMS, scanout or CPU mapping is provided.
#include <linux/dma-buf.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_gem.h>
#include "wddm_native_prime.h"
#include "wddm_render.h"

static bool experimental;
static bool enable_native_prime;
module_param(experimental, bool, 0444);
module_param(enable_native_prime, bool, 0444);
MODULE_PARM_DESC(enable_native_prime, "Enable bounded GEM handles for owned native DMA-BUFs only (default off)");
MODULE_PARM_DESC(experimental, "Explicit guest-only native render route (default off)");
static struct platform_device *render_parent;
static struct drm_device *render_device;
static atomic_t prime_held = ATOMIC_INIT(0);
static atomic64_t prime_created = ATOMIC64_INIT(0), prime_released = ATOMIC64_INIT(0);
struct native_gem { struct drm_gem_object gem; struct dma_buf *buffer; };
static void native_gem_free(struct drm_gem_object *gem)
{
    struct native_gem *native = container_of(gem, struct native_gem, gem);
    drm_gem_object_release(gem);
    dma_buf_put(native->buffer);
    kfree(native);
    atomic_dec(&prime_held); atomic64_inc(&prime_released);
}
static const struct drm_gem_object_funcs native_gem_funcs = { .free = native_gem_free };
static struct drm_gem_object *native_prime_import(struct drm_device *dev, struct dma_buf *buffer)
{
    typeof(&wddm_native_buffer_size) inspect;
    struct native_gem *native;
    u64 bytes = 0;
    int result;
    if (!enable_native_prime) return ERR_PTR(-EOPNOTSUPP);
    inspect = symbol_get(wddm_native_buffer_size);
    if (!inspect) return ERR_PTR(-ENODEV);
    result = inspect(buffer, &bytes);
    symbol_put(wddm_native_buffer_size);
    if (result) return ERR_PTR(result);
    if (!bytes || bytes > SIZE_MAX - (PAGE_SIZE - 1)) return ERR_PTR(-EINVAL);
    if (atomic_inc_return(&prime_held) > 256) { atomic_dec(&prime_held); return ERR_PTR(-ENOSPC); }
    native = kzalloc(sizeof(*native), GFP_KERNEL);
    if (!native) { atomic_dec(&prime_held); return ERR_PTR(-ENOMEM); }
    get_dma_buf(buffer); native->buffer = buffer;
    drm_gem_private_object_init(dev, &native->gem, PAGE_ALIGN(bytes));
    native->gem.funcs = &native_gem_funcs;
    atomic64_inc(&prime_created);
    return &native->gem;
}

static int render_info(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct wddm_render_info *info = data;
    int index;
    if (!drm_is_render_client(file))
        return -EACCES;
    if (info->version != WDDM_RENDER_VERSION || info->flags || info->signature ||
        info->max_width || info->max_height || info->reserved[0] || info->reserved[1])
        return -EINVAL;
    if (!drm_dev_enter(dev, &index))
        return -ENODEV;
    *info = (struct wddm_render_info) {
        .version = WDDM_RENDER_VERSION, .flags = WDDM_RENDER_NATIVE_ROUTE,
        .signature = WDDM_RENDER_MAGIC, .max_width = 4096, .max_height = 4096,
    };
    drm_dev_exit(index);
    return 0;
}
static const struct drm_ioctl_desc render_ioctls[] = {
    DRM_IOCTL_DEF_DRV(WDDM_RENDER_INFO, render_info, DRM_RENDER_ALLOW),
};
static long render_ioctl(struct file *file, unsigned int command, unsigned long argument)
{
    if (enable_native_prime && command == DRM_IOCTL_PRIME_FD_TO_HANDLE) {
        struct drm_prime_handle arg;
        if (copy_from_user(&arg, (void __user *)argument, sizeof(arg))) return -EFAULT;
        if (arg.flags) return -EINVAL;
    }
    if (command == DRM_IOCTL_VERSION || command == DRM_IOCTL_WDDM_RENDER_INFO ||
        (enable_native_prime && (command == DRM_IOCTL_PRIME_FD_TO_HANDLE || command == DRM_IOCTL_GEM_CLOSE)))
        return drm_ioctl(file, command, argument);
    if (command == DRM_IOCTL_GET_CAP) {
        struct drm_get_cap cap;
        void __user *out = (void __user *)argument;
        if (copy_from_user(&cap, out, sizeof(cap)))
            return -EFAULT;
        switch (cap.capability) {
        case DRM_CAP_DUMB_BUFFER:
        case DRM_CAP_SYNCOBJ:
        case DRM_CAP_SYNCOBJ_TIMELINE:
            cap.value = 0; /* These kernel interfaces are not implemented. */
            return copy_to_user(out, &cap, sizeof(cap)) ? -EFAULT : 0;
        case DRM_CAP_PRIME:
            cap.value = enable_native_prime ? DRM_PRIME_CAP_IMPORT : 0;
            return copy_to_user(out, &cap, sizeof(cap)) ? -EFAULT : 0;
        default:
            return -EOPNOTSUPP;
        }
    }
    return -ENOTTY;
}
static const struct file_operations render_fops = {
#ifdef FOP_UNSIGNED_OFFSET
    .fop_flags = FOP_UNSIGNED_OFFSET,
#endif
    .owner = THIS_MODULE, .open = drm_open, .release = drm_release,
    .unlocked_ioctl = render_ioctl,
#ifdef CONFIG_COMPAT
    .compat_ioctl = compat_ptr_ioctl,
#endif
    .poll = drm_poll, .read = drm_read, .llseek = noop_llseek,
};
static const struct drm_driver render_driver = {
    .driver_features = DRIVER_RENDER | DRIVER_GEM,
    .gem_prime_import = native_prime_import,
    .fops = &render_fops, .ioctls = render_ioctls,
    .num_ioctls = ARRAY_SIZE(render_ioctls),
    .name = "d3d12", .desc = "Private Windows-native D3D12 render route",
    .major = 1, .minor = 0,
};

static int __init render_init(void)
{
    int error;
    if (!experimental)
        return -EOPNOTSUPP;
    render_parent = platform_device_register_simple("wddm-render", PLATFORM_DEVID_NONE, NULL, 0);
    if (IS_ERR(render_parent))
        return PTR_ERR(render_parent);
    render_device = drm_dev_alloc(&render_driver, &render_parent->dev);
    if (IS_ERR(render_device)) {
        error = PTR_ERR(render_device);
        platform_device_unregister(render_parent);
        return error;
    }
    error = drm_dev_register(render_device, 0);
    if (error) {
        drm_dev_put(render_device);
        platform_device_unregister(render_parent);
        return error;
    }
    pr_info("WDDM_RENDER registered=true nativeRoute=true kms=false gem=%s gpuBackendVerified=false\n", enable_native_prime ? "opaque-import" : "false");
    if (enable_native_prime) pr_info("WDDM_RENDER_NATIVE_PRIME enabled=true limit=256 ownedExporterOnly=true cpuMap=false dmaAttach=false\n");
    return 0;
}
static void __exit render_exit(void)
{
    drm_dev_unregister(render_device);
    drm_dev_put(render_device);
    platform_device_unregister(render_parent);
    if (enable_native_prime) pr_info("WDDM_RENDER_NATIVE_PRIME_EXIT held=%d created=%lld released=%lld\n",
        atomic_read(&prime_held), atomic64_read(&prime_created), atomic64_read(&prime_released));
    pr_info("WDDM_RENDER unregistered=true\n");
}
module_init(render_init);
module_exit(render_exit);
MODULE_LICENSE("GPL");
/* Same namespace metadata on old token-style and new string-style kernels. */
MODULE_INFO(import_ns, "DMA_BUF");
MODULE_DESCRIPTION("Experimental guest render-node entry for the owned Windows D3D12 bridge");
