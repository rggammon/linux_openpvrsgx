/* SPDX-License-Identifier: GPL-2.0 */
/*
 * dc_nohw DMA-BUF exporter UAPI.
 *
 * Exposes the contiguous CMA swapchain back buffers (allocated by dc_nohw at
 * device init, ARGB8888) as DMA-BUF FDs through /dev/dc_nohw_export.
 */
#ifndef _UAPI_DC_NOHW_EXPORT_H_
#define _UAPI_DC_NOHW_EXPORT_H_

#include <linux/types.h>
#include <linux/ioctl.h>

#define DC_NOHW_EXPORT_ABI_VERSION 2u

/* DRM_FORMAT_ARGB8888 == fourcc('A','R','2','4') */
#define DC_NOHW_EXPORT_FOURCC_ARGB8888 0x34325241u

struct dc_nohw_export_abi {
	__u32 abi_version;
	__u32 width;
	__u32 height;
	__u32 stride;
	__u32 fourcc;
	__u32 buffer_count;
	__u32 buffer_size;
	__u32 reserved;
};

struct dc_nohw_export_buffer {
	__u32 index;    /* in  */
	__u32 flags;    /* in, must be 0 */
	__s32 fd;       /* out */
	__u32 reserved;
};

#define DC_NOHW_EXPORT_IOC_MAGIC 'D'
#define DC_NOHW_EXPORT_QUERY_ABI \
	_IOR(DC_NOHW_EXPORT_IOC_MAGIC, 1, struct dc_nohw_export_abi)
#define DC_NOHW_EXPORT_BUFFER \
	_IOWR(DC_NOHW_EXPORT_IOC_MAGIC, 2, struct dc_nohw_export_buffer)

/* --- Swap-notify (ABI v2): make the export fd pollable for swap events. --- */

struct dc_nohw_export_event {
        __u32 type;      /* DC_NOHW_EVENT_* */
        __u32 index;     /* SWAP: back-buffer index; SWAPCHAIN_CREATE: buffer_count */
        __u32 seq;       /* SWAP: monotonic sequence; else 0 */
        __u32 reserved;
};

#define DC_NOHW_EVENT_SWAPCHAIN_CREATE  1u
#define DC_NOHW_EVENT_SWAPCHAIN_DESTROY 2u
#define DC_NOHW_EVENT_SWAP              3u

/* Arm swap-notify on this fd, then poll()/read() struct dc_nohw_export_event. */
#define DC_NOHW_EXPORT_SUBSCRIBE \
        _IO(DC_NOHW_EXPORT_IOC_MAGIC, 3)

#endif /* _UAPI_DC_NOHW_EXPORT_H_ */

