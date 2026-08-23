# Copyright © 2023 Collabora
# SPDX-License-Identifier: MIT

libpankmod_lib_files = files('pan_kmod.c')

# The DRM-native backends compile only on platforms with a /dev/dri/cardN
# and a libdrm. Windows uses the WinMali D3DKMT-routed backend instead.
if host_machine.system() != 'windows'
  libpankmod_lib_files += files(
    'panfrost_kmod.c',
    'panthor_kmod.c',
  )
endif

if with_winmali_wddm_kmod
  libpankmod_lib_files += files(
    'winmali/winmali_kmod.c',
    'winmali/winmali_d3dkmt.c',
  )
endif

libpankmod_lib_inc = [inc_include, inc_src, inc_panfrost]
if with_winmali_wddm_kmod
  libpankmod_lib_inc += winmali_lib_inc
endif

libpankmod_lib = static_library(
  'pankmod_lib',
  [libpankmod_lib_files],
  include_directories : libpankmod_lib_inc,
  c_args : [no_override_init_args],
  gnu_symbol_visibility : 'hidden',
  dependencies: [dep_libdrm, idep_mesautil, idep_pan_packers, libpanfrost_model_dep],
  build_by_default : false,
)

libpankmod_dep = declare_dependency(
  include_directories: [inc_include, inc_src],
  dependencies: [dep_libdrm],
)
