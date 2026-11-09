// SPDX-License-Identifier: GPL-2.0

/*
 * Dxgkrnl Graphics Driver
 * virtio transport
 *
 * Replaces the VMBus channels, the vPCI adapter device and the VMBus MMIO
 * allocation with one virtio device per host adapter. The dxgkrnl message
 * layer above it is unchanged: messages are the dxgvmbus.h structures.
 *
 * One virtio-dxg device carries both the global channel (the first device
 * to probe owns it) and its adapter's channel. Requests are tagged with the
 * channel they belong to. Async messages are only ever queued, never waited
 * on, and keep their submission order on the single control queue.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/scatterlist.h>
#include <linux/completion.h>
#include <linux/wait.h>
#include <linux/ioport.h>
#include <linux/version.h>
#include <linux/virtio.h>
#include <linux/virtio_config.h>
#include "dxgkrnl.h"
#include "dxgvmbus.h"
#include "virtio_dxg.h"

#undef dev_fmt
#define dev_fmt(fmt)	"dxgk: " fmt

#define DXGVIRTIO_EVENT_BUFFERS	128

struct dxgvirtio_req {
	struct kref			ref;
	struct list_head		done_entry;
	struct completion		done;
	struct virtio_dxg_req		hdr;
	struct virtio_dxg_resp		resp;
	void				*buf;	/* message, then result */
	u32				cmd_size;
	u32				result_size;
	bool				async;
};

struct dxgvirtio {
	struct virtio_device		*vdev;
	struct virtqueue		*ctrl;
	struct virtqueue		*event;
	spinlock_t			ctrl_lock;
	wait_queue_head_t		ctrl_space;
	spinlock_t			event_lock;
	u8				*event_bufs;
	u32				event_size;
	u32				max_message;
	struct virtio_shm_region	iospace;
	struct resource			*iospace_res;
	struct dxgvgpuchannel		*vgpu_channel;
	guid_t				adapter_guid;
	struct winluid			vgpu_luid;
	bool				compute_only;
	bool				owns_global;
};

static void dxgvirtio_req_free(struct kref *ref)
{
	struct dxgvirtio_req *req = container_of(ref, struct dxgvirtio_req,
						 ref);

	kvfree(req->buf);
	kfree(req);
}

/*
 * Builds a scatterlist for a kmalloc'ed or vmalloc'ed buffer. Each page of a
 * vmalloc'ed buffer is a separate entry.
 */
static int dxgvirtio_sg_table(struct sg_table *table, void *buf, u32 size)
{
	struct scatterlist *sg;
	unsigned long first, last;
	u32 nents, i;
	int ret;

	if (!is_vmalloc_addr(buf)) {
		ret = sg_alloc_table(table, 1, GFP_KERNEL);
		if (ret)
			return ret;
		sg_set_buf(table->sgl, buf, size);
		return 0;
	}

	first = (unsigned long)buf >> PAGE_SHIFT;
	last = ((unsigned long)buf + size - 1) >> PAGE_SHIFT;
	nents = last - first + 1;
	ret = sg_alloc_table(table, nents, GFP_KERNEL);
	if (ret)
		return ret;
	for_each_sg(table->sgl, sg, nents, i) {
		u32 offset = offset_in_page(buf);
		u32 len = min_t(u32, size, PAGE_SIZE - offset);

		sg_set_page(sg, vmalloc_to_page(buf), len, offset);
		buf += len;
		size -= len;
	}
	return 0;
}

/* Retires the requests the host completed; returns how many. */
static unsigned int dxgvirtio_ctrl_reap(struct dxgvirtio *dv)
{
	struct virtqueue *vq = dv->ctrl;
	struct dxgvirtio_req *req, *next;
	unsigned long flags;
	unsigned int len, count = 0;
	LIST_HEAD(done);

	spin_lock_irqsave(&dv->ctrl_lock, flags);
	do {
		virtqueue_disable_cb(vq);
		while ((req = virtqueue_get_buf(vq, &len)) != NULL)
			list_add_tail(&req->done_entry, &done);
	} while (!virtqueue_enable_cb(vq));
	spin_unlock_irqrestore(&dv->ctrl_lock, flags);

	list_for_each_entry_safe(req, next, &done, done_entry) {
		list_del(&req->done_entry);
		if (!req->async)
			complete(&req->done);
		kref_put(&req->ref, dxgvirtio_req_free);
		count++;
	}
	if (count)
		wake_up_all(&dv->ctrl_space);
	return count;
}

static void dxgvirtio_ctrl_done(struct virtqueue *vq)
{
	dxgvirtio_ctrl_reap(vq->vdev->priv);
}

/*
 * A waiter that saw no completion for a while retires finished requests
 * itself: a completion whose interrupt was lost must not hang a process
 * (it did with INTx under WHPX). Reports that it happened.
 */
static void dxgvirtio_ctrl_poll(struct dxgvirtio *dv)
{
	unsigned int found = dxgvirtio_ctrl_reap(dv);

	if (found)
		pr_warn_ratelimited("dxgkrnl_virtio: %u completion(s) found without an interrupt\n",
				    found);
}

static int dxgvirtio_queue(struct dxgvirtio *dv, struct dxgvirtio_req *req)
{
	struct scatterlist hdr_sg, resp_sg, *sgs[4];
	struct sg_table cmd_table, res_table = {};
	unsigned int out = 0, in = 0;
	unsigned long flags;
	bool notify;
	int ret;

	ret = dxgvirtio_sg_table(&cmd_table, req->buf, req->cmd_size);
	if (ret)
		return ret;
	if (req->result_size) {
		ret = dxgvirtio_sg_table(&res_table,
					 (u8 *)req->buf + req->cmd_size,
					 req->result_size);
		if (ret)
			goto cleanup;
	}

	sg_init_one(&hdr_sg, &req->hdr, sizeof(req->hdr));
	sgs[out++] = &hdr_sg;
	sgs[out++] = cmd_table.sgl;
	if (!req->async) {
		sg_init_one(&resp_sg, &req->resp, sizeof(req->resp));
		sgs[out + in++] = &resp_sg;
		if (req->result_size)
			sgs[out + in++] = res_table.sgl;
	}

	for (;;) {
		spin_lock_irqsave(&dv->ctrl_lock, flags);
		ret = virtqueue_add_sgs(dv->ctrl, sgs, out, in, req,
					GFP_ATOMIC);
		notify = ret == 0 && virtqueue_kick_prepare(dv->ctrl);
		spin_unlock_irqrestore(&dv->ctrl_lock, flags);
		if (ret != -ENOSPC)
			break;
		/* Ring full: wait for the host to retire requests. */
		if (wait_event_killable_timeout(dv->ctrl_space,
						dv->ctrl->num_free >= out + in,
						HZ / 100) == 0)
			dxgvirtio_ctrl_poll(dv);
		if (fatal_signal_pending(current)) {
			ret = -EINTR;
			break;
		}
	}
	if (notify)
		virtqueue_notify(dv->ctrl);

cleanup:
	/* The ring holds DMA addresses; the tables themselves can go. */
	sg_free_table(&cmd_table);
	if (req->result_size)
		sg_free_table(&res_table);
	return ret;
}

static int dxgvirtio_status(u32 status)
{
	switch (status) {
	case VIRTIO_DXG_OK:
		return 0;
	case VIRTIO_DXG_ERR_UNSUPPORTED:
		return -EOPNOTSUPP;
	case VIRTIO_DXG_ERR_OVERFLOW:
		return -EOVERFLOW;
	case VIRTIO_DXG_ERR_INVALID:
		return -EINVAL;
	default:
		return -EIO;
	}
}

int dxgvirtio_send(struct dxgvirtio *dv, u32 channel, const void *command,
		   u32 cmd_size, void *result, u32 result_size, bool async)
{
	struct dxgvirtio_req *req;
	int ret;

	if (cmd_size == 0 || cmd_size > dv->max_message ||
	    result_size > dv->max_message || (async && result_size))
		return -EINVAL;

	req = kzalloc(sizeof(*req), GFP_KERNEL);
	if (!req)
		return -ENOMEM;
	req->buf = kvmalloc(cmd_size + result_size, GFP_KERNEL);
	if (!req->buf) {
		kfree(req);
		return -ENOMEM;
	}
	memcpy(req->buf, command, cmd_size);
	if (result_size)
		memset((u8 *)req->buf + cmd_size, 0, result_size);
	req->cmd_size = cmd_size;
	req->result_size = result_size;
	req->async = async;
	req->hdr.kind = cpu_to_le32(async ? VIRTIO_DXG_REQ_ASYNC :
					    VIRTIO_DXG_REQ_SYNC);
	req->hdr.channel = cpu_to_le32(channel);
	req->hdr.size = cpu_to_le32(cmd_size);
	req->hdr.result_size = cpu_to_le32(result_size);
	init_completion(&req->done);
	/* One reference for the submitter, one for the device. */
	kref_init(&req->ref);
	kref_get(&req->ref);

	ret = dxgvirtio_queue(dv, req);
	if (ret) {
		DXG_ERR("virtqueue_add_sgs failed: %d", ret);
		kref_put(&req->ref, dxgvirtio_req_free);
		goto out;
	}
	if (async)
		goto out;

	for (;;) {
		long left = wait_for_completion_killable_timeout(&req->done,
								 HZ / 10);

		if (left != 0) {
			ret = left < 0 ? left : 0;
			break;
		}
		dxgvirtio_ctrl_poll(dv);
	}
	if (ret) {
		/* The device still owns the buffers; it drops its reference. */
		DXG_ERR("wait_for_completion failed: %d", ret);
		goto out;
	}
	if (le32_to_cpu(req->resp.status) == VIRTIO_DXG_ERR_NTSTATUS) {
		/* Deliver the result (e.g. escape output) with the error. */
		ret = dxgvmb_ntstatus_to_errno(le32_to_cpu(req->resp.ntstatus));
		if (result_size)
			memcpy(result, (u8 *)req->buf + cmd_size, result_size);
		goto out;
	}
	ret = dxgvirtio_status(le32_to_cpu(req->resp.status));
	if (ret == 0 && le32_to_cpu(req->resp.size) < result_size) {
		DXG_TRACE("short reply %u, expected %u",
			  le32_to_cpu(req->resp.size), result_size);
		ret = -EOVERFLOW;
	}
	if (ret == 0 && result_size)
		memcpy(result, (u8 *)req->buf + cmd_size, result_size);
out:
	kref_put(&req->ref, dxgvirtio_req_free);
	return ret;
}

static int dxgvirtio_post_event(struct dxgvirtio *dv, void *buf)
{
	struct scatterlist sg;

	sg_init_one(&sg, buf, dv->event_size);
	return virtqueue_add_inbuf(dv->event, &sg, 1, buf, GFP_ATOMIC);
}

static void dxgvirtio_event_done(struct virtqueue *vq)
{
	struct dxgvirtio *dv = vq->vdev->priv;
	struct virtio_dxg_event *event;
	unsigned long flags;
	unsigned int len;
	bool notify = false;

	spin_lock_irqsave(&dv->event_lock, flags);
	do {
		virtqueue_disable_cb(vq);
		while ((event = virtqueue_get_buf(vq, &len)) != NULL) {
			u32 size = le32_to_cpu(event->size);

			if (len >= sizeof(*event) &&
			    size <= len - sizeof(*event))
				dxgvmb_process_host_packet((void *)&event[1],
							   size);
			else
				DXG_ERR("malformed host event %u/%u",
					size, len);
			if (dxgvirtio_post_event(dv, event) == 0)
				notify = true;
		}
	} while (!virtqueue_enable_cb(vq));
	if (notify)
		notify = virtqueue_kick_prepare(vq);
	spin_unlock_irqrestore(&dv->event_lock, flags);
	if (notify)
		virtqueue_notify(vq);
}

int dxgvirtio_get_iospace(struct dxgvirtio *dv, u64 *base, u64 *size)
{
	if (!dv->iospace.len)
		return -ENOMEM;
	*base = dv->iospace.addr;
	*size = dv->iospace.len;
	return 0;
}

static int dxgvirtio_find_vqs(struct dxgvirtio *dv)
{
	struct virtqueue *vqs[VIRTIO_DXG_VQ_COUNT];
	int ret;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 11, 0)
	struct virtqueue_info info[VIRTIO_DXG_VQ_COUNT] = {
		[VIRTIO_DXG_VQ_CTRL] = { "ctrl", dxgvirtio_ctrl_done },
		[VIRTIO_DXG_VQ_EVENT] = { "event", dxgvirtio_event_done },
	};

	ret = virtio_find_vqs(dv->vdev, VIRTIO_DXG_VQ_COUNT, vqs, info, NULL);
#else
	vq_callback_t *callbacks[VIRTIO_DXG_VQ_COUNT] = {
		dxgvirtio_ctrl_done, dxgvirtio_event_done,
	};
	static const char * const names[VIRTIO_DXG_VQ_COUNT] = {
		"ctrl", "event",
	};

	ret = virtio_find_vqs(dv->vdev, VIRTIO_DXG_VQ_COUNT, vqs, callbacks,
			      names, NULL);
#endif
	if (ret)
		return ret;
	dv->ctrl = vqs[VIRTIO_DXG_VQ_CTRL];
	dv->event = vqs[VIRTIO_DXG_VQ_EVENT];
	return 0;
}

static void dxgvirtio_read_config(struct dxgvirtio *dv)
{
	struct virtio_device *vdev = dv->vdev;
	u32 low, high, flags;

	virtio_cread_le(vdev, struct virtio_dxg_config, vgpu_luid_low, &low);
	virtio_cread_le(vdev, struct virtio_dxg_config, vgpu_luid_high, &high);
	virtio_cread_le(vdev, struct virtio_dxg_config, flags, &flags);
	virtio_cread_le(vdev, struct virtio_dxg_config, max_message,
			&dv->max_message);
	virtio_cread_le(vdev, struct virtio_dxg_config, event_size,
			&dv->event_size);
	virtio_cread_bytes(vdev, offsetof(struct virtio_dxg_config,
					  adapter_guid),
			   &dv->adapter_guid, sizeof(dv->adapter_guid));
	dv->vgpu_luid.a = low;
	dv->vgpu_luid.b = high;
	dv->compute_only = flags & VIRTIO_DXG_CFG_COMPUTE_ONLY;
	if (dv->max_message == 0 ||
	    dv->max_message > DXG_MAX_VM_BUS_PACKET_SIZE)
		dv->max_message = DXG_MAX_VM_BUS_PACKET_SIZE;
	if (dv->event_size < sizeof(struct virtio_dxg_event) +
			     sizeof(struct dxgkvmb_command_host_to_vm) ||
	    dv->event_size > PAGE_SIZE)
		dv->event_size = VIRTIO_DXG_EVENT_SIZE;
}

/*
 * The first device sets the protocol version and owns the global channel
 * and the IO space. Later devices only add adapters.
 */
static int dxgvirtio_negotiate(struct dxgvirtio *dv)
{
	struct dxgglobal *dxgglobal = dxggbl();
	/* dxgk_vmbus_guestcaps.wsl2 and dxgk_vmbus_hostcaps.map_guest_memory */
	u32 guest_caps = 1u << 0;
	u32 host_caps;
	u32 version;

	virtio_cwrite_le(dv->vdev, struct virtio_dxg_config, guest_caps,
			 &guest_caps);
	if (dxgglobal->hdev)
		return 0;

	virtio_cread_le(dv->vdev, struct virtio_dxg_config, vmbus_version,
			&version);
	virtio_cread_le(dv->vdev, struct virtio_dxg_config, host_caps,
			&host_caps);
	if (version < DXGK_VMBUS_INTERFACE_VERSION) {
		/* The extended header and in-band NTSTATUS are required. */
		DXG_ERR("host interface %u is older than %u", version,
			DXGK_VMBUS_INTERFACE_VERSION);
		return -ENODEV;
	}
	dxgglobal->vmbus_ver = DXGK_VMBUS_INTERFACE_VERSION;
	dxgglobal->map_guest_pages_enabled =
		(host_caps & (1u << 0)) &&
		virtio_has_feature(dv->vdev, VIRTIO_DXG_F_GUEST_PAGES);

	if (!virtio_get_shm_region(dv->vdev, &dv->iospace,
				   VIRTIO_DXG_SHM_IOSPACE)) {
		DXG_ERR("the host offers no IO space");
		return -ENODEV;
	}
	dv->iospace_res = request_mem_region(dv->iospace.addr,
					     dv->iospace.len,
					     "dxgkrnl-iospace");
	if (!dv->iospace_res) {
		DXG_ERR("IO space %llx+%llx is busy", dv->iospace.addr,
			dv->iospace.len);
		return -EBUSY;
	}
	dv->owns_global = true;
	return 0;
}

static int dxgvirtio_probe(struct virtio_device *vdev)
{
	struct dxgglobal *dxgglobal = dxggbl();
	struct dxgvgpuchannel *vgpuch;
	struct dxgvirtio *dv;
	u32 i;
	int ret;

	dv = kzalloc(sizeof(*dv), GFP_KERNEL);
	if (!dv)
		return -ENOMEM;
	dv->vdev = vdev;
	vdev->priv = dv;
	spin_lock_init(&dv->ctrl_lock);
	spin_lock_init(&dv->event_lock);
	init_waitqueue_head(&dv->ctrl_space);
	dxgvirtio_read_config(dv);

	ret = dxgvirtio_find_vqs(dv);
	if (ret)
		goto free_dv;

	dv->event_bufs = kcalloc(DXGVIRTIO_EVENT_BUFFERS, dv->event_size,
				 GFP_KERNEL);
	if (!dv->event_bufs) {
		ret = -ENOMEM;
		goto del_vqs;
	}

	mutex_lock(&dxgglobal->device_mutex);
	ret = dxgvirtio_negotiate(dv);
	if (ret)
		goto unlock;

	virtio_device_ready(vdev);
	for (i = 0; i < DXGVIRTIO_EVENT_BUFFERS; i++)
		dxgvirtio_post_event(dv, dv->event_bufs + i * dv->event_size);
	virtqueue_kick(dv->event);

	ret = dxgglobal_create_adapter(NULL, &dv->adapter_guid, dv->vgpu_luid,
				       dv->compute_only);
	if (ret)
		goto reset;

	vgpuch = kzalloc(sizeof(*vgpuch), GFP_KERNEL);
	if (!vgpuch) {
		ret = -ENOMEM;
		goto reset;
	}
	guid_to_luid(&dv->adapter_guid, &vgpuch->adapter_luid);
	vgpuch->hdev = dv;
	dv->vgpu_channel = vgpuch;
	list_add_tail(&vgpuch->vgpu_ch_list_entry,
		      &dxgglobal->vgpu_ch_list_head);
	if (dv->owns_global)
		dxgglobal->hdev = dv;
	dxgglobal_start_adapters();
	mutex_unlock(&dxgglobal->device_mutex);

	DXG_TRACE("virtio-dxg adapter %x-%x, iospace %llx+%llx",
		  dv->vgpu_luid.b, dv->vgpu_luid.a, dv->iospace.addr,
		  dv->iospace.len);
	return 0;

reset:
	virtio_reset_device(vdev);
	if (dv->iospace_res)
		release_mem_region(dv->iospace.addr, dv->iospace.len);
unlock:
	mutex_unlock(&dxgglobal->device_mutex);
	kfree(dv->event_bufs);
del_vqs:
	vdev->config->del_vqs(vdev);
free_dv:
	kfree(dv);
	return ret;
}

static void dxgvirtio_remove(struct virtio_device *vdev)
{
	struct dxgvirtio *dv = vdev->priv;
	struct dxgglobal *dxgglobal = dxggbl();
	struct dxgvirtio_req *req;

	mutex_lock(&dxgglobal->device_mutex);
	dxgglobal_remove_transport(dv, &dv->adapter_guid);
	if (dv->vgpu_channel) {
		list_del(&dv->vgpu_channel->vgpu_ch_list_entry);
		kfree(dv->vgpu_channel);
	}
	if (dv->owns_global)
		dxgglobal_destroy_global_channel();
	mutex_unlock(&dxgglobal->device_mutex);

	virtio_reset_device(vdev);
	/* Requests nobody waits for any more; waiters were woken above. */
	while ((req = virtqueue_detach_unused_buf(dv->ctrl)) != NULL) {
		if (!req->async) {
			req->resp.status = cpu_to_le32(VIRTIO_DXG_ERR_DEVICE);
			complete(&req->done);
		}
		kref_put(&req->ref, dxgvirtio_req_free);
	}
	vdev->config->del_vqs(vdev);
	if (dv->iospace_res)
		release_mem_region(dv->iospace.addr, dv->iospace.len);
	kfree(dv->event_bufs);
	kfree(dv);
}

static const struct virtio_device_id dxgvirtio_id_table[] = {
	{ VIRTIO_ID_DXG, VIRTIO_DEV_ANY_ID },
	{ 0 },
};
MODULE_DEVICE_TABLE(virtio, dxgvirtio_id_table);

static const unsigned int dxgvirtio_features[] = {
	VIRTIO_DXG_F_GUEST_PAGES,
};

static struct virtio_driver dxgvirtio_driver = {
	.driver.name		= KBUILD_MODNAME,
	.id_table		= dxgvirtio_id_table,
	.feature_table		= dxgvirtio_features,
	.feature_table_size	= ARRAY_SIZE(dxgvirtio_features),
	.probe			= dxgvirtio_probe,
	.remove			= dxgvirtio_remove,
};

int dxgvirtio_register(void)
{
	return register_virtio_driver(&dxgvirtio_driver);
}

void dxgvirtio_unregister(void)
{
	unregister_virtio_driver(&dxgvirtio_driver);
}
