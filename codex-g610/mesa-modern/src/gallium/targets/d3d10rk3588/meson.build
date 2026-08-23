# Copyright © 2026 WinMali project
# SPDX-License-Identifier: MIT
#
# WinMali D3D10/11 UMD target. Same shape as the existing d3d10umd
# target, but our d3d10_create_screen() returns the panfrost-on-
# WinMali screen so the d3d10umd frontend ends up driving Mali-G610.

# Reuse d3d10umd's .def.in - the exported entry points are identical
# (OpenAdapter10[_2] + the D3DKMT thunk surface).
d3d10_rk3588_def = custom_target(
  'd3d10_rk3588.def',
  input  : '../d3d10umd/d3d10.def.in',
  output : 'd3d10_rk3588.def',
  command : gen_vs_module_defs_normal_command,
)

gallium_d3d10_name = get_option('gallium-d3d10-dll-name')

libgallium_d3d10_rk3588 = shared_library(
  gallium_d3d10_name,
  ['d3d10_rk3588.c'],
  vs_module_defs : d3d10_rk3588_def,
  include_directories : [
    inc_include, inc_src, inc_gallium, inc_gallium_aux,
    inc_d3d10umd, inc_gallium_winsys, inc_gallium_drivers,
    inc_winddk, inc_panfrost,
  ],
  link_whole : [libd3d10umd],
  link_with  : [
    libgallium, libpanfrost_winmali_winsys,
  ],
  dependencies : [
    dep_ws2_32, idep_nir, idep_mesautil, driver_panfrost,
  ],
  name_prefix : '',
  install : true,
)
