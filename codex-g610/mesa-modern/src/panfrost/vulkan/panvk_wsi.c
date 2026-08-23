/*
 * Copyright © 2021 Collabora Ltd.
 * Copyright © 2025 Arm Ltd.
 *
 * Derived from tu_wsi.c:
 * Copyright © 2016 Red Hat
 * Copyright © 2015 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#include "panvk_wsi.h"
#include "panvk_instance.h"
#include "panvk_physical_device.h"

#include "vk_util.h"
#include "wsi_common.h"

static VKAPI_PTR PFN_vkVoidFunction
panvk_wsi_proc_addr(VkPhysicalDevice physicalDevice, const char *pName)
{
   VK_FROM_HANDLE(panvk_physical_device, pdevice, physicalDevice);
   struct panvk_instance *instance = to_panvk_instance(pdevice->vk.instance);

   return vk_instance_get_proc_addr_unchecked(&instance->vk, pName);
}

static bool
panvk_can_present_on_device(VkPhysicalDevice pdevice, int fd)
{
   drmDevicePtr device;
   if (drmGetDevice2(fd, 0, &device) != 0)
      return false;
   /* Allow on-device presentation for all devices with bus type PLATFORM.
    * Other device types such as PCI or USB should use the PRIME blit path. */
   bool match = device->bustype == DRM_BUS_PLATFORM;

   drmFreeDevice(&device);

   return match;
}

VkResult
panvk_wsi_init(struct panvk_physical_device *physical_device)
{
   struct panvk_instance *instance =
      to_panvk_instance(physical_device->vk.instance);
   VkResult result;

   /* sw_device=true on Windows. Without it, mesa's wsi_common_win32
    * brings up an IDXGIFactory4 + DCompositionDevice at WSI init time
    * and allocates swap-chain backbuffers as ID3D12Resource objects.
    * That path requires our adapter to also expose a working D3D12
    * driver, which we don't (and have no plan to ship). With sw_device
    * the WSI uses the GDI bitmap fallback in user-mode:
    *
    *    panvk renders into a regular VkImage on the GPU
    *      -> wsi_common copies the image into a HOST_VISIBLE VkBuffer
    *      -> user-mode reads the buffer pointer
    *      -> SetDIBitsToDevice to the HWND's HDC
    *
    * The "sw" in sw_device refers only to the WSI image-allocation
    * strategy (linear, CPU-readable) - panvk still runs the actual
    * draw on the Mali. No software rasterizer is involved. See
    * docs/MESA_INTEGRATION.md "Vulkan WSI strategy" for the full
    * story. */
#if defined(_WIN32) && defined(HAVE_WINMALI_WDDM_KMOD)
   const bool wsi_sw = true;
#else
   const bool wsi_sw = false;
#endif

   result = wsi_device_init(&physical_device->wsi_device,
                            panvk_physical_device_to_handle(physical_device),
                            panvk_wsi_proc_addr, &instance->vk.alloc, -1,
                            &instance->dri_options,
                            &(struct wsi_device_options){.sw_device = wsi_sw});
   if (result != VK_SUCCESS)
      return result;

   physical_device->wsi_device.supports_modifiers = true;
   physical_device->wsi_device.can_present_on_device =
      panvk_can_present_on_device;

   physical_device->vk.wsi_device = &physical_device->wsi_device;

   return VK_SUCCESS;
}

void
panvk_wsi_finish(struct panvk_physical_device *physical_device)
{
   struct panvk_instance *instance =
      to_panvk_instance(physical_device->vk.instance);

   physical_device->vk.wsi_device = NULL;
   wsi_device_finish(&physical_device->wsi_device, &instance->vk.alloc);
}
