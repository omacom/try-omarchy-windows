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
	uint32_t capacity = t->capacity ? t->capacity * 2 : 1024;
	struct dxghost_obj *slots = calloc(capacity, sizeof(*slots));
	uint32_t i;

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

	if (!handle)
		return -1;
	if ((t->used + 1) * 2 > t->capacity && objtab_grow(t))
		return -1;
	if (dxghost_obj_find(h, handle))
		return -1;	/* dxgkrnl never hands out a live handle twice */
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

/* Fence pages: one read-only guest mapping per host page, refcounted. */

uint64_t dxghost_fence_map(struct dxghost *h, void *fence_hva)
{
	uintptr_t va = (uintptr_t)fence_hva;
	void *page = (void *)(va & ~(uintptr_t)(PAGE_SIZE_4K - 1));
	uint64_t offset = va & (PAGE_SIZE_4K - 1);
	struct dxghost_fence_page *p;
	uint32_t i;
	uint64_t gpa;

	if (!fence_hva)
		return 0;
	for (i = 0; i < h->fence_page_count; i++) {
		if (h->fence_pages[i].hva == page) {
			h->fence_pages[i].refs++;
			return h->fence_pages[i].gpa + offset;
		}
	}
	if (h->fence_page_count == h->fence_page_capacity) {
		uint32_t capacity = h->fence_page_capacity ?
				    h->fence_page_capacity * 2 : 64;
		p = realloc(h->fence_pages, capacity * sizeof(*p));
		if (!p)
			return 0;
		h->fence_pages = p;
		h->fence_page_capacity = capacity;
	}
	gpa = dxghost_io_alloc(h, PAGE_SIZE_4K);
	if (!gpa)
		return 0;
	/* Fence values are written by the GPU and read by the guest CPU. */
	if (h->vmm.map(h->vmm.opaque, gpa, page, PAGE_SIZE_4K, false)) {
		dxghost_io_free(h, gpa, PAGE_SIZE_4K);
		return 0;
	}
	p = &h->fence_pages[h->fence_page_count++];
	p->hva = page;
	p->gpa = gpa;
	p->refs = 1;
	return gpa + offset;
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
		h->vmm.unmap(h->vmm.opaque, gpa, PAGE_SIZE_4K);
		dxghost_io_free(h, gpa, PAGE_SIZE_4K);
		*p = h->fence_pages[--h->fence_page_count];
		return;
	}
}
