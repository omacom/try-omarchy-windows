/* SPDX-License-Identifier: MIT */
#ifndef DXG_KMT_BRIDGE_H
#define DXG_KMT_BRIDGE_H

#include <stdint.h>

/* The guest's uapi d3dkmthk.h, outside the kernel: pointers are __u64. */
typedef uint8_t __u8;
typedef uint16_t __u16;
typedef uint32_t __u32;
typedef uint64_t __u64;
typedef int8_t __s8;
typedef int16_t __s16;
typedef int32_t __s32;
typedef int64_t __s64;
#define __packed __attribute__((packed))
#ifndef _IOWR
#define _IOWR(type, nr, T) (0xc0000000u | ((unsigned)sizeof(T) << 16) | \
			    ((type) << 8) | (nr))
#endif
#include "../guest/include/uapi/misc/d3dkmthk.h"

#include <wchar.h>

/*
 * Loads dxgkmt.so (Windows path) and opens /dev/dxg. trace: 1 logs every
 * call, 0 failures only, -1 nothing. Returns 0 or -errno.
 */
int kmt_bridge_open(const wchar_t *unix_lib_path, int trace);
/* The bridge function for a "D3DKMTxxx" name, or NULL. */
void *kmt_bridge_lookup(const char *d3dkmt_name);
void kmt_bridge_report(void);
/*
 * A Linux descriptor of a shared GPU object as an NT handle Windows code can
 * hold, and back (-1 when the handle is not one of these).
 */
void *kmt_bridge_fd_handle(int fd);
int kmt_bridge_handle_fd(const void *handle);
/* A dxgkmt_unix.h call into dxgkmt.so: 0, or -EIO when it did not run. */
int kmt_bridge_call(unsigned int code, void *args);
/*
 * Display devices backed by the dxg adapters (kmt_display.c): wrappers for
 * user32's EnumDisplayDevicesA/W by name, and the D3DKMT calls that map a
 * DC or GDI display name to an adapter.
 */
void *kmt_display_resolve(const char *user32_name);
int32_t __attribute__((stdcall)) kmt_display_open_from_hdc(void *args);
int32_t __attribute__((stdcall)) kmt_display_open_from_name(void *args);
/* Like kmt_bridge_lookup, but never NULL for a D3DKMT name: unbridged
 * functions get a logging STATUS_NOT_SUPPORTED stub. */
void *kmt_bridge_resolve(const char *d3dkmt_name);

#endif
