# Copyright © 2026 WinMali project
# SPDX-License-Identifier: MIT

libpanfrost_winmali_winsys = static_library(
  'panfrost_winmali_winsys',
  files('panfrost_winmali_winsys.c'),
  include_directories : [
    inc_src, inc_include,
    inc_gallium, inc_gallium_aux, inc_gallium_drivers,
    inc_panfrost,
    winmali_lib_inc,
  ],
  gnu_symbol_visibility : 'hidden',
  dependencies : [idep_mesautil],
)
