# Copyright (C) 2020 Collabora, Ltd.
# SPDX-License-Identifier: MIT

TEMPLATE = """
#ifndef _BI_OPCODES_H_
#define _BI_OPCODES_H_

#include "bifrost.h"
 
% for mod in sorted(modifiers):
% if len(modifiers[mod]) > 2: # otherwise just boolean
enum bi_${mod.lower()} {
% for i, state in enumerate(modifiers[mod]):
% if state != "reserved":
    BI_${mod.upper()}_${state.upper()} = ${i},
% endif
% endfor
};

% endif
% endfor
enum bi_opcode {
% for opcode in sorted(mnemonics):
    BI_OPCODE_${opcode.replace('.', '_').upper()},
% endfor
    BI_NUM_OPCODES
};

/* Number of staging registers accessed, note this fits into 3-bits */

enum bi_sr_count {
    /* fixed counts */
    BI_SR_COUNT_0 = 0,
    BI_SR_COUNT_1 = 1,
    BI_SR_COUNT_2 = 2,
    BI_SR_COUNT_3 = 3,
    BI_SR_COUNT_4 = 4,

    /* derived from register_format and vecsize */
    BI_SR_COUNT_FORMAT = 5,

    /* equal to vecsize alone */
    BI_SR_COUNT_VECSIZE = 6,

    /* specified directly as the sr_count immediate */
    BI_SR_COUNT_SR_COUNT = 7
};

enum bi_size {
   BI_SIZE_8 = 0,
   BI_SIZE_16,
   BI_SIZE_24,
   BI_SIZE_32,
   BI_SIZE_48,
   BI_SIZE_64,
   BI_SIZE_96,
   BI_SIZE_128,
};

/* Description of an opcode in the IR */
struct bi_op_props {
        const char *name;

        /* All bitfields share one uint32_t storage unit. MSVC, unlike
         * GCC/Clang, starts a new storage unit whenever the declared
         * type of a bitfield changes (enum -> bool -> unsigned), which
         * inflates this struct from 4 bytes to 12 and rotates the
         * generated static-initializer values onto wrong fields. */
        uint32_t message    : 4;  /* enum bifrost_message_type */
        uint32_t size       : 3;  /* enum bi_size */
        uint32_t sr_count   : 3;  /* enum bi_sr_count */
        uint32_t sr_read    : 1;
        uint32_t sr_write   : 1;
        uint32_t last       : 1;
        uint32_t branch     : 1;
        uint32_t table      : 1;
        uint32_t fma        : 1;
        uint32_t add        : 1;
        uint32_t is_float   : 1;

        /* Supported propagable modifiers */
        uint32_t clamp      : 1;
        uint32_t not_result : 1;
        uint32_t abs        : 3;
        uint32_t neg        : 3;
        uint32_t not_mod    : 1;
};

/* Generated in bi_opcodes.c.py */
extern struct bi_op_props bi_opcode_props[BI_NUM_OPCODES];

#endif
"""

import sys
from bifrost_isa import *
from mako.template import Template

instructions = {}
for arg in sys.argv[1:]:
    new_instructions = parse_instructions(arg, include_pseudo = True)
    instructions.update(new_instructions)

ir_instructions = partition_mnemonics(instructions)
modifier_lists = order_modifiers(ir_instructions)

# Generate sorted list of mnemonics without regard to unit
mnemonics = set(x[1:] for x in instructions.keys())

print(Template(COPYRIGHT + TEMPLATE).render(mnemonics = mnemonics, modifiers = modifier_lists))
