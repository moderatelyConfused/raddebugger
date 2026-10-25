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
};

////////////////////////////////
//~ rjf: Module Info

typedef struct UWND_ModuleInfo UWND_ModuleInfo;
struct UWND_ModuleInfo
{
  U64 base_vaddr;
  void *unwind_info;
};

////////////////////////////////
//~ rjf: arm64 Prologue / Epilogue Analysis

#if defined(ARM64_H)
typedef enum UWND_ARM64_CodeState
{
  UWND_ARM64_CodeState_Body,     // the function's frame is established; the regular unwind rule applies
  UWND_ARM64_CodeState_Prologue, // before the frame is set up: lr holds the return address, fp the caller's fp
  UWND_ARM64_CodeState_Epilogue, // after the frame record was restored: lr holds the return address, fp the caller's fp
}
UWND_ARM64_CodeState;
internal UWND_ARM64_CodeState uwnd_arm64_code_state_from_pc(MemoryMap *memory_map, U64 function_vaddr, U64 pc, U64 sp, U64 *cfa_out);
#endif

////////////////////////////////
//~ rjf: Moduleless Unwinder

internal UWND_StepResult uwnd_step_moduleless(Arch arch, MemoryMap *memory_map, void *regs);

////////////////////////////////
//~ rjf: Abstracted Unwind Step Functions

internal UWND_StepResult uwnd_step(UWND_Unwinder unwinder, Arch arch, MemoryMap *memory_map, UWND_ModuleInfo *module_info, U64 tls_vaddr, void *regs, U64 *cfa_out);

#endif // UNWIND_H
