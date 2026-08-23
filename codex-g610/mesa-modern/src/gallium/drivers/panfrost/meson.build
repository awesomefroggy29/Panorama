# Copyright © 2017 Intel Corporation
# Copyright © 2018 Alyssa Rosenzweig
# SPDX-License-Identifier: MIT

files_panfrost = files(
  'driinfo_panfrost.h',
  'pan_mod_conv_cso.c',
  'pan_bo.c',
  'pan_device.c',
  'pan_disk_cache.c',
  'pan_fence.c',
  'pan_helpers.c',
  'pan_public.h',
  'pan_screen.c',
  'pan_screen.h',
  'pan_resource.c',
  'pan_resource.h',
  'pan_context.c',
  'pan_blend_cso.c',
  'pan_blit.c',
  'pan_job.c',
  'pan_shader.c',
  'pan_mempool.c',
  'pan_mempool.h',
  'pan_nir_remove_fragcolor_stores.c',
  'pan_nir_lower_pls.c',
  'pan_nir_lower_sysvals.c',
  'pan_nir_lower_res_indices.c',
)

panfrost_includes = [
  inc_gallium,
  inc_gallium_aux,
  inc_include,
  inc_src,
  inc_panfrost,
]

# WinMali WDDM bridge: rk3588-GPU/Shared/ carries WinMaliWddmBridge.h, used by
# pan_resource.c's panfrost_winmali_set_wddm_alloc_ops.
if with_winmali_wddm_kmod
  panfrost_includes += winmali_lib_inc
endif

compile_args_panfrost = [
  '-DGALLIUM_PANFROST',
  '-Wno-pointer-arith'
]

panfrost_versions = ['4', '5', '6', '7', '9', '10', '12', '13', '14']
libpanfrost_versions = []

foreach ver : panfrost_versions
  files_panfrost_vx = [
    'pan_cmdstream.c',
    'pan_blend_cso.c',
    'pan_fb_preload.c',
    'pan_precomp.c',
    pan_packers,
  ]
  if ver in ['4', '5', '6', '7', '9']
    files_panfrost_vx += ['pan_jm.c']
  elif ver in ['10', '12', '13', '14']
    files_panfrost_vx += ['pan_csf.c']
  endif
  libpanfrost_versions += static_library(
    'panfrost-v' + ver, files_panfrost_vx,
    include_directories : panfrost_includes,
    c_args : ['-DPAN_ARCH=' + ver],
    gnu_symbol_visibility : 'hidden',
    dependencies : [idep_pan_packers, idep_nir, dep_libdrm, idep_mesautil, idep_libpan_per_arch[ver]],
)
endforeach

libpanfrost = static_library(
  'panfrost',
  files_panfrost,
  dependencies: [
    dep_thread,
    dep_libdrm,
    idep_mesautil,
    idep_nir,
    idep_pan_packers
  ],
  include_directories : panfrost_includes,
  c_args : [c_msvc_compat_args, compile_args_panfrost],
  gnu_symbol_visibility : 'hidden',
  link_with: [libpanfrost_versions],
)

# Pick whichever winsys variant was built for this host. On Linux that's
# the DRM winsys; on Windows + WinMali it's the D3DKMT-routed one.
if with_winmali_wddm_kmod
  _panfrost_winsys_lib = libpanfrost_winmali_winsys
else
  _panfrost_winsys_lib = libpanfrostwinsys
endif

driver_panfrost = declare_dependency(
  compile_args : compile_args_panfrost,
  link_with : [libpanfrost, _panfrost_winsys_lib, libpanfrost_shared, libpanfrost_compiler, libpanfrost_decode, libpanfrost_lib],
  dependencies : [idep_xmlconfig],
)
