// SPDX-License-Identifier: GPL-2.0
/*
 * Handle table, IO-space allocator and fence-page mappings.
 *
 * Callers hold host->lock exclusively. Requests of one guest process run in
 * order (dxghost_lane), so a handle validated here cannot be destroyed by a
 * concurrent request before it is used.
 */
#include <stdlib.h>
#include <string.h>
#include "host.h"

#define PAGE_SIZE_4K 4096ull

static uint32_t slot_of(uint32_t handle, uint32_t capacity)
{
	return (handle * 2654435761u) & (capacity - 1);
}

static int objtab_grow(struct dxghost_objtab *t)
{
	uint32_t capacity = t->capacity ? t->capacity : 1024;
	struct dxghost_obj *slots;
	uint32_t i;

	/* Deleted slots trigger a rebuild, not necessarily growth. Keeping the
	 * size tied to live handles bounds churn costs when apps open/close. */
	if (t->capacity && ((uint64_t)t->live + 1) * 2 > capacity) {
		if (capacity > UINT32_MAX / 2)
			return -1;
		capacity *= 2;
	}
	slots = calloc(capacity, sizeof(*slots));
	if (!slots)
		return -1;
	for (i = 0; i < t->capacity; i++) {
		struct dxghost_obj *o = &t->slots[i];
		uint32_t s;

		if (o->state != 1)
			continue;
		for (s = slot_of(o->handle, capacity); slots[s].state;
		     s = (s + 1) & (capacity - 1))
			;
		slots[s] = *o;
	}
	free(t->slots);
	t->slots = slots;
	t->capacity = capacity;
	t->used = t->live;
	return 0;
}

int dxghost_obj_add(struct dxghost *h, uint32_t handle, uint8_t type,
		    uint32_t process, uint32_t parent)
{
	struct dxghost_objtab *t = &h->objects;
	struct dxghost_obj *o;
	uint32_t s;

	if (!handle || dxghost_obj_find(h, handle))
		return -1;
	if (((uint64_t)t->used + 1) * 2 > t->capacity && objtab_grow(t))
		return -1;
	for (s = slot_of(handle, t->capacity); t->slots[s].state == 1;
	     s = (s + 1) & (t->capacity - 1))
		;
	o = &t->slots[s];
	if (o->state == 0)
		t->used++;
	memset(o, 0, sizeof(*o));
	o->handle = handle;
	o->type = type;
	o->state = 1;
	o->process = process;
	o->parent = parent;
	t->live++;
	return 0;
}

struct dxghost_obj *dxghost_obj_find(struct dxghost *h, uint32_t handle)
{
	struct dxghost_objtab *t = &h->objects;
	uint32_t s;

	if (!handle || !t->capacity)
		return NULL;
	for (s = slot_of(handle, t->capacity); t->slots[s].state;
	     s = (s + 1) & (t->capacity - 1))
		if (t->slots[s].state == 1 && t->slots[s].handle == handle)
			return &t->slots[s];
	return NULL;
}

struct dxghost_obj *dxghost_obj_get(struct dxghost *h, uint32_t handle,
				    uint8_t type, uint32_t process)
{
	struct dxghost_obj *o = dxghost_obj_find(h, handle);

	if (!o || o->type != type || o->process != process)
		return NULL;
	return o;
}

void dxghost_obj_remove(struct dxghost *h, uint32_t handle)
{
	struct dxghost_obj *o = dxghost_obj_find(h, handle);

	if (!o)
		return;
	o->state = 2;
	h->objects.live--;
}

void dxghost_objtab_free(struct dxghost_objtab *t)
{
	free(t->slots);
	memset(t, 0, sizeof(*t));
}

/* IO space: a sorted free list, first fit, coalescing on free. */

void dxghost_io_reset(struct dxghost *h, uint64_t base, uint64_t size)
{
	h->io_base = base;
	h->io_size = size;
	h->io_free_count = 0;
	if (!size)
		return;
	if (!h->io_free_capacity) {
		h->io_free = calloc(64, sizeof(*h->io_free));
		if (!h->io_free)
			return;
		h->io_free_capacity = 64;
	}
	h->io_free[0].start = base;
	h->io_free[0].size = size;
	h->io_free_count = 1;
}

uint64_t dxghost_io_alloc(struct dxghost *h, uint64_t size)
{
	/* Large mappings stay 2 MiB aligned so the guest can use huge pages. */
	uint64_t align = size >= (2ull << 20) ? (2ull << 20) : PAGE_SIZE_4K;
	uint32_t i;

	size = (size + PAGE_SIZE_4K - 1) & ~(PAGE_SIZE_4K - 1);
	for (i = 0; i < h->io_free_count; i++) {
		struct dxghost_range *r = &h->io_free[i];
		uint64_t start = (r->start + align - 1) & ~(align - 1);
		uint64_t pad = start - r->start;

		if (pad + size > r->size)
			continue;
		if (pad) {
			/* Keep the alignment gap as its own free range. */
			uint64_t rest = r->size - pad - size;

			r->size = pad;
			if (rest)
				dxghost_io_free(h, start + size, rest);
		} else {
			r->start += size;
			r->size -= size;
			if (!r->size) {
				memmove(r, r + 1, (h->io_free_count - i - 1) *
						  sizeof(*r));
				h->io_free_count--;
			}
		}
		return start;
	}
	return 0;
}

void dxghost_io_free(struct dxghost *h, uint64_t gpa, uint64_t size)
{
	struct dxghost_range *r;
	uint32_t i;

	size = (size + PAGE_SIZE_4K - 1) & ~(PAGE_SIZE_4K - 1);
	for (i = 0; i < h->io_free_count && h->io_free[i].start < gpa; i++)
		;
	/* Merge with the previous and/or next range. */
	if (i > 0 && h->io_free[i - 1].start + h->io_free[i - 1].size == gpa) {
		r = &h->io_free[i - 1];
		r->size += size;
		if (i < h->io_free_count &&
		    r->start + r->size == h->io_free[i].start) {
			r->size += h->io_free[i].size;
			memmove(&h->io_free[i], &h->io_free[i + 1],
				(h->io_free_count - i - 1) * sizeof(*r));
			h->io_free_count--;
		}
		return;
	}
	if (i < h->io_free_count && gpa + size == h->io_free[i].start) {
		h->io_free[i].start = gpa;
		h->io_free[i].size += size;
		return;
	}
	if (h->io_free_count == h->io_free_capacity) {
		uint32_t capacity = h->io_free_capacity * 2;
		struct dxghost_range *grown =
			realloc(h->io_free, capacity * sizeof(*grown));

		if (!grown) {
			dxghost_logf(h, "IO space free list exhausted; "
					"leaking %llu bytes",
				     (unsigned long long)size);
			return;
		}
		h->io_free = grown;
		h->io_free_capacity = capacity;
	}
	memmove(&h->io_free[i + 1], &h->io_free[i],
		(h->io_free_count - i) * sizeof(*h->io_free));
	h->io_free[i].start = gpa;
	h->io_free[i].size = size;
	h->io_free_count++;
}

/* h->lock and the work deferred under it (lock order: see host.h). */

void dxghost_lock(struct dxghost *h)
{
	AcquireSRWLockExclusive(&h->lock);
}

static struct dxghost_deferred *defer_slot(struct dxghost *h)
{
	if (h->deferred_count == h->deferred_capacity) {
		uint32_t capacity = h->deferred_capacity ?
				    h->deferred_capacity * 2 : 16;
		struct dxghost_deferred *grown =
			realloc(h->deferred, capacity * sizeof(*grown));

		if (!grown)
			return NULL;
		h->deferred = grown;
		h->deferred_capacity = capacity;
	}
	return &h->deferred[h->deferred_count++];
}

void dxghost_defer_unmap(struct dxghost *h, uint64_t gpa, uint64_t size)
{
	struct dxghost_deferred *d = defer_slot(h);

	if (!d) {
		/* Leave it mapped and its IO space reserved: never reused. */
		dxghost_logf(h, "out of memory deferring an unmap of %llu bytes",
			     (unsigned long long)size);
		return;
	}
	d->fn = NULL;
	d->arg = NULL;
	d->gpa = gpa;
	d->size = size;
}

void dxghost_defer_call(struct dxghost *h, void (*fn)(void *), void *arg)
{
	struct dxghost_deferred *d = defer_slot(h);

	if (!d) {
		dxghost_logf(h, "out of memory deferring a close");
		return;		/* leaked: never run under the lock */
	}
	d->fn = fn;
	d->arg = arg;
	d->gpa = d->size = 0;
}

void dxghost_unlock(struct dxghost *h)
{
	struct dxghost_deferred *work = h->deferred;
	uint32_t count = h->deferred_count, i;
	bool unmapped = false;

	if (!count) {
		ReleaseSRWLockExclusive(&h->lock);
		return;
	}
	h->deferred = NULL;
	h->deferred_count = h->deferred_capacity = 0;
	ReleaseSRWLockExclusive(&h->lock);
	/* Unmaps first, as one batch where the VMM supports it. */
	for (i = 0; i < count; i++) {
		if (work[i].fn)
			continue;
		if (!unmapped && h->vmm.batch)
			h->vmm.batch(h->vmm.opaque, true);
		h->vmm.unmap(h->vmm.opaque, work[i].gpa, work[i].size);
		unmapped = true;
	}
	if (unmapped && h->vmm.batch)
		h->vmm.batch(h->vmm.opaque, false);
	/* Closes wait for callbacks that enter the VMM: outside the batch. */
	for (i = 0; i < count; i++)
		if (work[i].fn)
			work[i].fn(work[i].arg);
	/* Only now may the IO space hold another view. */
	if (unmapped) {
		AcquireSRWLockExclusive(&h->lock);
		for (i = 0; i < count; i++)
			if (!work[i].fn)
				dxghost_io_free(h, work[i].gpa, work[i].size);
		ReleaseSRWLockExclusive(&h->lock);
	}
	free(work);
}

/* Fence pages: one read-only guest mapping per host page, refcounted. */

uint64_t dxghost_fence_map(struct dxghost *h, void *fence_hva)
{
	uintptr_t va = (uintptr_t)fence_hva;
	void *page = (void *)(va & ~(uintptr_t)(PAGE_SIZE_4K - 1));
	uint64_t offset = va & (PAGE_SIZE_4K - 1);
	struct dxghost_fence_page *p;
	uint32_t i;
	uint64_t gpa = 0;

	if (!fence_hva)
		return 0;
	AcquireSRWLockExclusive(&h->map_lock);
	dxghost_lock(h);
	for (i = 0; i < h->fence_page_count; i++) {
		if (h->fence_pages[i].hva == page) {
			h->fence_pages[i].refs++;
			gpa = h->fence_pages[i].gpa + offset;
			dxghost_unlock(h);
			ReleaseSRWLockExclusive(&h->map_lock);
			return gpa;
		}
	}
	if (h->fence_page_count == h->fence_page_capacity) {
		uint32_t capacity = h->fence_page_capacity ?
				    h->fence_page_capacity * 2 : 64;

		p = realloc(h->fence_pages, capacity * sizeof(*p));
		if (p) {
			h->fence_pages = p;
			h->fence_page_capacity = capacity;
		}
	}
	if (h->fence_page_count < h->fence_page_capacity)
		gpa = dxghost_io_alloc(h, PAGE_SIZE_4K);
	dxghost_unlock(h);
	/* Fence values are written by the GPU and read by the guest CPU. */
	if (gpa && h->vmm.map(h->vmm.opaque, gpa, page, PAGE_SIZE_4K, false)) {
		dxghost_lock(h);
		dxghost_io_free(h, gpa, PAGE_SIZE_4K);
		dxghost_unlock(h);
		gpa = 0;
	}
	if (gpa) {
		/* map_lock kept the page out of the table meanwhile, and only
		 * unmaps (which never add) ran: the slot is still free. */
		dxghost_lock(h);
		p = &h->fence_pages[h->fence_page_count++];
		p->hva = page;
		p->gpa = gpa;
		p->refs = 1;
		dxghost_unlock(h);
	}
	ReleaseSRWLockExclusive(&h->map_lock);
	return gpa ? gpa + offset : 0;
}

void dxghost_fence_unmap(struct dxghost *h, uint64_t fence_gpa)
{
	uint64_t gpa = fence_gpa & ~(PAGE_SIZE_4K - 1);
	uint32_t i;

	if (!fence_gpa)
		return;
	for (i = 0; i < h->fence_page_count; i++) {
		struct dxghost_fence_page *p = &h->fence_pages[i];

		if (p->gpa != gpa)
			continue;
		if (--p->refs)
			return;
		dxghost_defer_unmap(h, gpa, PAGE_SIZE_4K);
		*p = h->fence_pages[--h->fence_page_count];
		return;
	}
}
