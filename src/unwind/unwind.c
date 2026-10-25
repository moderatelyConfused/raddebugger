// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

////////////////////////////////
//~ rjf: Generated Code

#include "unwind/generated/unwind.meta.c"

////////////////////////////////
//~ rjf: arm64 Prologue / Epilogue Analysis
//
// Frame-based unwind rules (compact unwind info in "frame" mode, or the frame
// pointer chain) describe a function only once its prologue has stored the
// frame record and pointed fp at it. They are wrong at the first instructions
// of a function (e.g. right after stepping into a call), and in the epilogue
// once the frame record has been restored. In both of those states the return
// address is still in lr and the caller's fp is in fp; only the entry stack
// pointer has to be recovered, by replaying the stack adjustments the prologue
// has made so far, or those the epilogue has still to make.

#if defined(ARM64_H)

typedef enum UWND_ARM64_InstKind
{
  UWND_ARM64_InstKind_Other,
  UWND_ARM64_InstKind_Nop,    // nop, pointer authentication hints
  UWND_ARM64_InstKind_SpSub,  // sub sp, sp, #imm
  UWND_ARM64_InstKind_SpAdd,  // add sp, sp, #imm
  UWND_ARM64_InstKind_Store,  // str/stp of x or d registers to [sp, #imm] (sp_delta < 0 when pre-indexed)
  UWND_ARM64_InstKind_Load,   // ldr/ldp of x or d registers from [sp, #imm] (sp_delta > 0 when post-indexed)
  UWND_ARM64_InstKind_SetFp,  // add x29, sp, #imm (mov x29, sp)
  UWND_ARM64_InstKind_Return, // ret, retaa, retab
  UWND_ARM64_InstKind_Branch, // b imm (a tail call, when the epilogue has already popped the stack)
}
UWND_ARM64_InstKind;

typedef struct UWND_ARM64_Inst UWND_ARM64_Inst;
struct UWND_ARM64_Inst
{
  UWND_ARM64_InstKind kind;
  S64 sp_delta;      // bytes added to sp by the instruction's writeback
  B32 touches_frame; // a load/store which includes x29 or x30
};

internal UWND_ARM64_Inst
uwnd_arm64_inst_from_code(U32 code)
{
  UWND_ARM64_Inst inst = {UWND_ARM64_InstKind_Other};
  U32 rt  = (code & 0x1f);
  U32 rn  = (code >> 5) & 0x1f;
  U32 rt2 = (code >> 10) & 0x1f;
  
  //- rjf: nop, pacibsp, paciasp, autibsp, autiasp
  if(code == 0xd503201f || code == 0xd503237f || code == 0xd503233f || code == 0xd50323ff || code == 0xd50323bf)
  {
    inst.kind = UWND_ARM64_InstKind_Nop;
  }
  
  //- rjf: ret, retaa, retab
  else if(code == 0xd65f03c0 || code == 0xd65f0bff || code == 0xd65f0fff)
  {
    inst.kind = UWND_ARM64_InstKind_Return;
  }
  
  //- rjf: b imm26
  else if((code & 0xfc000000) == 0x14000000)
  {
    inst.kind = UWND_ARM64_InstKind_Branch;
  }
  
  //- rjf: add/sub sp, sp, #imm{, lsl #12}
  else if((code & 0xbf8003ff) == 0x910003ff)
  {
    U64 imm = (code >> 10) & 0xfff;
    if(code & (1u<<22)) { imm <<= 12; }
    B32 is_sub = !!(code & 0x40000000);
    inst.kind = is_sub ? UWND_ARM64_InstKind_SpSub : UWND_ARM64_InstKind_SpAdd;
    inst.sp_delta = is_sub ? -(S64)imm : (S64)imm;
  }
  
  //- rjf: add x29, sp, #imm (mov x29, sp)
  else if((code & 0xff8003ff) == 0x910003fd)
  {
    inst.kind = UWND_ARM64_InstKind_SetFp;
  }
  
  //- rjf: stp/ldp {x,d}t1, {x,d}t2, [sp, #imm] / [sp, #imm]! / [sp], #imm
  else if(rn == 31 && (code & 0x38000000) == 0x28000000 && ((code >> 23) & 7) != 0)
  {
    U32 addressing = (code >> 23) & 7; // 1: post-index, 2: signed offset, 3: pre-index
    U32 is_load    = (code >> 22) & 1;
    U32 is_vector  = (code >> 26) & 1;
    U32 opc        = (code >> 30) & 3;
    U64 scale      = is_vector ? (4u << opc) : (opc == 2 ? 8 : 4);
    S64 imm7       = (S64)((code >> 15) & 0x7f);
    if(imm7 & 0x40) { imm7 -= 0x80; }
    inst.kind          = is_load ? UWND_ARM64_InstKind_Load : UWND_ARM64_InstKind_Store;
    inst.sp_delta      = (addressing == 2) ? 0 : imm7 * (S64)scale;
    inst.touches_frame = (!is_vector && (rt == 29 || rt == 30 || rt2 == 29 || rt2 == 30));
  }
  
  //- rjf: str/ldr {x,d}t, [sp, #imm] (unsigned offset)
  else if(rn == 31 && (code >> 30) == 3 && (code & 0x3b000000) == 0x39000000 && ((code >> 22) & 3) < 2)
  {
    inst.kind          = ((code >> 22) & 1) ? UWND_ARM64_InstKind_Load : UWND_ARM64_InstKind_Store;
    inst.touches_frame = (!((code >> 26) & 1) && (rt == 29 || rt == 30));
  }
  
  //- rjf: str/ldr {x,d}t, [sp, #imm]! / [sp], #imm
  else if(rn == 31 && (code >> 30) == 3 && (code & 0x3b200400) == 0x38000400 && ((code >> 22) & 3) < 2)
  {
    S64 imm9 = (S64)((code >> 12) & 0x1ff);
    if(imm9 & 0x100) { imm9 -= 0x200; }
    inst.kind          = ((code >> 22) & 1) ? UWND_ARM64_InstKind_Load : UWND_ARM64_InstKind_Store;
    inst.sp_delta      = imm9;
    inst.touches_frame = (!((code >> 26) & 1) && (rt == 29 || rt == 30));
  }
  return inst;
}

internal B32
uwnd_arm64_inst_is_prologue(UWND_ARM64_Inst inst)
{
  return (inst.kind == UWND_ARM64_InstKind_SpSub || inst.kind == UWND_ARM64_InstKind_Store || inst.kind == UWND_ARM64_InstKind_Nop);
}

internal UWND_ARM64_CodeState
uwnd_arm64_code_state_from_pc(MemoryMap *memory_map, U64 function_vaddr, U64 pc, U64 sp, U64 *cfa_out)
{
  UWND_ARM64_CodeState state = UWND_ARM64_CodeState_Body;
  U64 cfa = 0;
  U32 code[32] = {0};
  U64 code_bytes = sizeof(code);
  
  //////////////////////////////
  //- rjf: prologue, known function start -> replay the instructions from the
  // start up to pc; if all of them are prologue instructions, and none has set
  // fp, the frame is not established yet
  //
  if(function_vaddr != 0 && function_vaddr <= pc && pc - function_vaddr <= code_bytes && (pc & 3) == 0)
  {
    U64 count = (pc - function_vaddr)/4;
    if(count == 0 || memory_map_read(memory_map, r1u64(function_vaddr, pc), code) == pc - function_vaddr)
    {
      S64 depth = 0;
      B32 in_prologue = 1;
      for EachIndex(idx, count)
      {
        UWND_ARM64_Inst inst = uwnd_arm64_inst_from_code(code[idx]);
        if(!uwnd_arm64_inst_is_prologue(inst))
        {
          in_prologue = 0;
          break;
        }
        depth -= inst.sp_delta;
      }
      if(in_prologue)
      {
        state = UWND_ARM64_CodeState_Prologue;
        cfa = sp + depth;
      }
    }
  }
  
  //////////////////////////////
  //- rjf: prologue, unknown function start -> if a frame setup lies ahead of
  // pc, reachable only through prologue instructions, then pc is inside a
  // prologue; the prologue instructions immediately behind pc are the ones
  // which have already executed (the previous function ends with a return,
  // branch, or trap, which stops the backwards scan)
  //
  else if(function_vaddr == 0 && (pc & 3) == 0)
  {
    U64 fwd_bytes = memory_map_read(memory_map, r1u64(pc, pc + code_bytes), code);
    B32 setup_ahead = 0;
    for(U64 idx = 0; idx < fwd_bytes/4; idx += 1)
    {
      UWND_ARM64_Inst inst = uwnd_arm64_inst_from_code(code[idx]);
      if(inst.kind == UWND_ARM64_InstKind_SetFp) { setup_ahead = 1; break; }
      if(!uwnd_arm64_inst_is_prologue(inst)) { break; }
    }
    if(setup_ahead)
    {
      S64 depth = 0;
      for(U64 window = code_bytes; window >= 4 && pc >= window; window /= 2)
      {
        if(memory_map_read(memory_map, r1u64(pc - window, pc), code) == window)
        {
          for(U64 idx = window/4; idx > 0; idx -= 1)
          {
            UWND_ARM64_Inst inst = uwnd_arm64_inst_from_code(code[idx-1]);
            if(!uwnd_arm64_inst_is_prologue(inst)) { break; }
            depth -= inst.sp_delta;
          }
          break;
        }
      }
      state = UWND_ARM64_CodeState_Prologue;
      cfa = sp + depth;
    }
  }
  
  //////////////////////////////
  //- rjf: epilogue -> if a return (or a tail call, once the stack was popped)
  // lies ahead of pc, reachable only through stack pops and register restores
  // which do not touch the frame record, then the frame record has already
  // been restored, and the remaining pops give the entry stack pointer
  //
  if(state == UWND_ARM64_CodeState_Body && (pc & 3) == 0)
  {
    U64 fwd_bytes = memory_map_read(memory_map, r1u64(pc, pc + code_bytes), code);
    S64 remaining = 0;
    B32 popped_any = 0;
    for(U64 idx = 0; idx < fwd_bytes/4; idx += 1)
    {
      UWND_ARM64_Inst inst = uwnd_arm64_inst_from_code(code[idx]);
      if(inst.kind == UWND_ARM64_InstKind_Return || (inst.kind == UWND_ARM64_InstKind_Branch && popped_any))
      {
        state = UWND_ARM64_CodeState_Epilogue;
        cfa = sp + remaining;
        break;
      }
      else if(inst.kind == UWND_ARM64_InstKind_Load && !inst.touches_frame)
      {
        remaining += inst.sp_delta;
        popped_any = (popped_any || inst.sp_delta != 0);
      }
      else if(inst.kind == UWND_ARM64_InstKind_SpAdd)
      {
        remaining += inst.sp_delta;
        popped_any = 1;
      }
      else if(inst.kind != UWND_ARM64_InstKind_Nop)
      {
        break;
      }
    }
  }
  
  if(cfa_out != 0)
  {
    cfa_out[0] = cfa;
  }
  return state;
}

#endif

////////////////////////////////
//~ rjf: Moduleless Unwinder

internal UWND_StepResult
uwnd_step_moduleless(Arch arch, MemoryMap *memory_map, void *regs)
{
  UWND_StepResult result = {UWND_StepStatus_Error};
#if defined(ARM64_H)
  //- rjf: arm64 -> follow the frame pointer chain (the arm64 ABI requires
  // frame pointers: [fp] = caller's fp, [fp+8] = return address), unless pc
  // is in a prologue or epilogue, where the frame record is not in place
  if(arch == Arch_arm64)
  {
    ARM64_RegBlock *regs_arm64 = (ARM64_RegBlock *)regs;
    U64 fp = regs_arm64->fp;
    U64 code_cfa = 0;
    UWND_ARM64_CodeState code_state = uwnd_arm64_code_state_from_pc(memory_map, 0, regs_arm64->pc, regs_arm64->sp, &code_cfa);
    if(code_state != UWND_ARM64_CodeState_Body)
    {
      regs_arm64->pc = regs_arm64->lr;
      regs_arm64->sp = code_cfa;
      result.status = UWND_StepStatus_Good;
    }
    else if(fp != 0 && (fp & 0xf) == 0)
    {
      U64 frame[2] = {0};
      Rng1U64 frame_vaddr_range = r1u64(fp, fp+16);
      if(memory_map_read(memory_map, frame_vaddr_range, frame) != 16)
      {
        result.status = UWND_StepStatus_FailedMemoryRead;
        result.missed_read_vaddr_range = frame_vaddr_range;
      }
      else
      {
        regs_arm64->fp = frame[0];
        regs_arm64->lr = frame[1];
        regs_arm64->pc = frame[1];
        regs_arm64->sp = fp + 16;
        result.status = UWND_StepStatus_Good;
      }
    }
    return result;
  }
#endif
  {
    B32 done = 0;
    ARCH_Info *arch_info = arch_info_from_arch(arch);
    U64 addr_size = byte_size_from_arch(arch);
    
    // rjf: read new ip from stack pointer
    U64 sp = arch_sp_from_reg_block(arch_info, regs);
    U64 new_ip = 0;
    Rng1U64 new_ip_vaddr_range = r1u64(sp, sp+addr_size);
    if(memory_map_read(memory_map, new_ip_vaddr_range, &new_ip) != addr_size)
    {
      done = 1;
      result.status = UWND_StepStatus_FailedMemoryRead;
      result.missed_read_vaddr_range = new_ip_vaddr_range;
    }
    
    // rjf: commit
    if(!done)
    {
      U64 new_sp = sp+addr_size;
      arch_reg_block_write_ip(arch_info, regs, new_ip);
      arch_reg_block_write_sp(arch_info, regs, new_sp);
      result.status = UWND_StepStatus_Good;
    }
  }
  return result;
}

////////////////////////////////
//~ rjf: Abstracted Unwind Step Functions

internal UWND_StepResult
uwnd_step(UWND_Unwinder unwinder, Arch arch, MemoryMap *memory_map, UWND_ModuleInfo *module_info, U64 tls_vaddr, void *regs, U64 *cfa_out)
{
  UWND_StepResult result = {UWND_StepStatus_Error};
  switch(unwinder)
  {
    default:{result = uwnd_step_moduleless(arch, memory_map, regs);}break;
#define X(name, namespace_lower) case UWND_Unwinder_##name:{result = namespace_lower##_uwnd_step(arch, memory_map, module_info, tls_vaddr, regs, cfa_out);}break;
    UWND_Unwinder_XList
#undef X
  }
  return result;
}
