// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#ifndef ARM64_H
#define ARM64_H

////////////////////////////////
//~ rjf: PSTATE / CPSR Flags

typedef U32 ARM64_PState;
enum
{
  ARM64_PState_N    = (1u << 31),
  ARM64_PState_Z    = (1u << 30),
  ARM64_PState_C    = (1u << 29),
  ARM64_PState_V    = (1u << 28),
  ARM64_PState_SS   = (1u << 21), // software step (SPSR.SS)
  ARM64_PState_IL   = (1u << 20),
  ARM64_PState_D    = (1u << 9),
  ARM64_PState_A    = (1u << 8),
  ARM64_PState_I    = (1u << 7),
  ARM64_PState_F    = (1u << 6),
};

////////////////////////////////
//~ rjf: Debug State (MDSCR_EL1 / breakpoint & watchpoint control registers)

#define ARM64_MDSCR_EL1_SS  (1u << 0)  // software step enable
#define ARM64_MDSCR_EL1_KDE (1u << 13) // kernel debug enable
#define ARM64_MDSCR_EL1_MDE (1u << 15) // monitor debug events enable

// DBGBCR<n>_EL1 / DBGWCR<n>_EL1 fields
#define ARM64_DBGBCR_E          (1u << 0)
#define ARM64_DBGBCR_PMC_EL0    (2u << 1)  // privilege mode control: EL0 only
#define ARM64_DBGBCR_BAS_ALL    (0xfu << 5) // byte address select: all 4 bytes
#define ARM64_DBGWCR_E          (1u << 0)
#define ARM64_DBGWCR_PAC_EL0    (2u << 1)
#define ARM64_DBGWCR_LSC_LOAD   (1u << 3)
#define ARM64_DBGWCR_LSC_STORE  (2u << 3)
#define ARM64_DBGWCR_BAS_SHIFT  5

////////////////////////////////
//~ rjf: Exception Syndrome Register (ESR_EL1) Exception Classes

typedef enum ARM64_ESR_EC
{
  ARM64_ESR_EC_Unknown             = 0x00,
  ARM64_ESR_EC_WFx                 = 0x01,
  ARM64_ESR_EC_SVE                 = 0x19,
  ARM64_ESR_EC_IllegalState        = 0x0e,
  ARM64_ESR_EC_SVC64               = 0x15,
  ARM64_ESR_EC_MSR                 = 0x18,
  ARM64_ESR_EC_InstAbortLower      = 0x20,
  ARM64_ESR_EC_InstAbortCurrent    = 0x21,
  ARM64_ESR_EC_PCAlignment         = 0x22,
  ARM64_ESR_EC_DataAbortLower      = 0x24,
  ARM64_ESR_EC_DataAbortCurrent    = 0x25,
  ARM64_ESR_EC_SPAlignment         = 0x26,
  ARM64_ESR_EC_FPException         = 0x2c,
  ARM64_ESR_EC_SError              = 0x2f,
  ARM64_ESR_EC_BreakpointLower     = 0x30,
  ARM64_ESR_EC_BreakpointCurrent   = 0x31,
  ARM64_ESR_EC_SoftwareStepLower   = 0x32,
  ARM64_ESR_EC_SoftwareStepCurrent = 0x33,
  ARM64_ESR_EC_WatchpointLower     = 0x34,
  ARM64_ESR_EC_WatchpointCurrent   = 0x35,
  ARM64_ESR_EC_BRK                 = 0x3c,
}
ARM64_ESR_EC;

#define ARM64_ESR_EC_FROM_ESR(esr) ((ARM64_ESR_EC)(((esr) >> 26) & 0x3f))
#define ARM64_ESR_ISS_FROM_ESR(esr) ((U32)((esr) & 0x1ffffff))
#define ARM64_ESR_DataAbort_WnR (1u << 6) // write-not-read bit in data abort ISS

////////////////////////////////
//~ rjf: Instruction Encodings

#define ARM64_INST_BRK0 0xd4200000u // brk #0
#define ARM64_INST_SIZE 4

////////////////////////////////
//~ rjf: Generated

#include "arm64/generated/arm64.meta.h"

////////////////////////////////
//~ rjf: Helpers

internal B32 arm64_inst_is_brk(U32 inst);
internal U32 arm64_brk_imm_from_inst(U32 inst);

#endif // ARM64_H
