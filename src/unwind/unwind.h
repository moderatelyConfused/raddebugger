// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#ifndef UNWIND_H
#define UNWIND_H

////////////////////////////////
//~ rjf: Generated Code

#include "unwind/generated/unwind.meta.h"

////////////////////////////////
//~ rjf: Unwind Artifact Types

typedef enum UWND_StepStatus
{
  UWND_StepStatus_Error,
  UWND_StepStatus_FailedMemoryRead,
  UWND_StepStatus_Good,
}
UWND_StepStatus;

typedef struct UWND_StepResult UWND_StepResult;
struct UWND_StepResult
{
  UWND_StepStatus status;
  Rng1U64 missed_read_vaddr_range;
  B32 is_signal_frame; // the frame stepped from is a signal trampoline (CIE augmentation "S"): the new pc is the interrupted instruction, not a return address
};

////////////////////////////////
//~ rjf: Module Info

typedef struct UWND_ModuleInfo UWND_ModuleInfo;
struct UWND_ModuleInfo
{
  U64 base_vaddr;
  void *unwind_info;
  B32 pc_is_return_address; // an outer frame: pc is the return address of a call, so the call's own address (pc-1) selects the unwind rule, and the code at pc has not executed
  B32 pc_code_unreadable;   // the code at pc could not be read: no prologue / epilogue analysis
  U64 function_vaddr;       // the start of the function containing pc, from debug info (0: unknown). Preferred to the unwind info's start (compact unwind entries are shared by runs of same-encoding functions), and all there is for functions the unwind info does not describe (syscall stubs)
};

////////////////////////////////
//~ rjf: arm64 Prologue / Epilogue Analysis

#if defined(ARM64_H)
typedef enum UWND_ARM64_CodeState
{
  UWND_ARM64_CodeState_Body,     // the function's frame is established; the regular unwind rule applies
  UWND_ARM64_CodeState_Prologue, // before the frame is set up: lr holds the return address, fp the caller's fp, and no callee-saved register has been modified
  UWND_ARM64_CodeState_Epilogue, // after the frame record was restored: lr holds the return address, fp the caller's fp; the callee-saved registers whose pops still lie ahead come from their slots (`pending`)
  UWND_ARM64_CodeState_NeedRead, // the code around pc is not in the memory map; `missed` is the range to read
}
UWND_ARM64_CodeState;

// rjf: a callee-saved register which an epilogue has still to restore, and its stack slot
typedef struct UWND_ARM64_RegSlot UWND_ARM64_RegSlot;
struct UWND_ARM64_RegSlot
{
  U32 reg;   // 19-28: xN; 32+8 - 32+15: vN (the low 64 bits)
  U64 vaddr; // the 8-byte slot
};

typedef struct UWND_ARM64_Analysis UWND_ARM64_Analysis;
struct UWND_ARM64_Analysis
{
  UWND_ARM64_CodeState state;
  U64 cfa;                        // Prologue / Epilogue: the entry stack pointer
  Rng1U64 missed;                 // NeedRead: the range to read
  U64 pending_count;              // Epilogue: the registers still to be restored by the code ahead of pc
  UWND_ARM64_RegSlot pending[18]; // x19-x28 and d8-d15 at most
};

internal U64 uwnd_arm64_stub_inst_count_from_code(U32 *code, U64 count);
internal UWND_ARM64_Analysis uwnd_arm64_analysis_from_pc(MemoryMap *memory_map, U64 function_vaddr, U64 pc, U64 sp);
internal UWND_ARM64_CodeState uwnd_arm64_code_state_from_pc(MemoryMap *memory_map, U64 function_vaddr, U64 pc, U64 sp, U64 *cfa_out, Rng1U64 *missed_out);
internal UWND_ARM64_Analysis uwnd_arm64_analysis_from_module_info(MemoryMap *memory_map, UWND_ModuleInfo *module_info, U64 function_vaddr, ARM64_RegBlock *regs);
internal UWND_StepResult uwnd_arm64_restore_pending(MemoryMap *memory_map, ARM64_RegBlock *regs, UWND_ARM64_Analysis *analysis);
internal UWND_StepResult uwnd_arm64_step_frame_chain(MemoryMap *memory_map, ARM64_RegBlock *regs, UWND_ARM64_Analysis *analysis);
#endif

////////////////////////////////
//~ rjf: Moduleless Unwinder

internal UWND_StepResult uwnd_step_moduleless(Arch arch, MemoryMap *memory_map, UWND_ModuleInfo *module_info, void *regs);

////////////////////////////////
//~ rjf: Abstracted Unwind Step Functions

internal UWND_StepResult uwnd_step(UWND_Unwinder unwinder, Arch arch, MemoryMap *memory_map, UWND_ModuleInfo *module_info, U64 tls_vaddr, void *regs, U64 *cfa_out);

#endif // UNWIND_H
