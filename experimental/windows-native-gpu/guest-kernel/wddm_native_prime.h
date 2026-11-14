/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef WDDM_NATIVE_PRIME_H
#define WDDM_NATIVE_PRIME_H
#ifdef __KERNEL__
struct dma_buf;
/* Exporter identity and capability validity, never a name or user ticket. */
int wddm_native_buffer_size(struct dma_buf *buffer, u64 *bytes);
#endif
#endif
