/*
 * Copyright © 2026 WinMali project
 * SPDX-License-Identifier: MIT
 *
 * Link-time stubs for symbols that live in libdrm-gated mesa code.
 *
 * On Linux, vk_drm_syncobj.c (in vulkan/runtime) implements these by
 * driving the panthor render-node fd through DRM_IOCTL_SYNCOBJ_*; that
 * TU is gated on dep_libdrm.found() in vulkan/runtime/meson.build, so
 * it's absent on Windows. We supply minimal stubs here. Runtime calls
 * into these routes fail fast — the WinMali ABI carries its own
 * sync-object opcodes (see WinMaliEscape.h) which the panvk gpu_queue
 * code wires up directly through pan_kmod_ioctl.
 */
#ifdef _WIN32

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "vk_sync.h"
#include "vk_drm_syncobj.h"

/* Returned by-value into panvk_physical_device::drm_syncobj_type. The
 * vk runtime treats a zeroed vk_sync_type as "this type is not
 * supported" and falls back to vk_sync_timeline. The signalled-only
 * surface is enough for the panvk physical-device init path to advance
 * past the syncobj-or-bust check. */
struct vk_sync_type
vk_drm_syncobj_get_type(int drm_fd)
{
   (void)drm_fd;
   struct vk_sync_type type;
   memset(&type, 0, sizeof(type));
   return type;
}

void
vk_drm_syncobj_finish(struct vk_device *device, struct vk_sync *sync)
{
   (void)device; (void)sync;
}

VkResult
vk_drm_syncobj_copy_payloads(struct vk_device *device,
                             uint32_t wait_count,
                             const struct vk_sync_wait *waits,
                             uint32_t signal_count,
                             const struct vk_sync_signal *signals)
{
   (void)device; (void)wait_count; (void)waits;
   (void)signal_count; (void)signals;
   return VK_ERROR_FEATURE_NOT_PRESENT;
}

#endif /* _WIN32 */
