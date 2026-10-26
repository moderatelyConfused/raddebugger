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
  UWND_ARM64_InstKind_Unknown,    // not decoded: may write sp, x29 or x30, trap, or call; ends every analysis
  UWND_ARM64_InstKind_Other,      // writes none of sp, x29, x30, and continues at the next instruction
  UWND_ARM64_InstKind_Nop,        // hints: nop, bti, pointer authentication
  UWND_ARM64_InstKind_SpSub,      // sub sp, sp, #imm
  UWND_ARM64_InstKind_SpAdd,      // add sp, sp, #imm
  UWND_ARM64_InstKind_Store,      // str/stp of x or d registers to [sp, #imm] (sp_delta < 0 when pre-indexed)
  UWND_ARM64_InstKind_Load,       // ldr/ldp of x or d registers from [sp, #imm] (sp_delta > 0 when post-indexed)
  UWND_ARM64_InstKind_SetFp,      // add x29, sp, #imm (mov x29, sp)
  UWND_ARM64_InstKind_Return,     // ret, retaa, retab
  UWND_ARM64_InstKind_Branch,     // b imm, br xN (a tail call, when the epilogue has already popped the stack)
  UWND_ARM64_InstKind_CondBranch, // b.cond, cbz, cbnz, tbz, tbnz (fall through when not taken)
}
UWND_ARM64_InstKind;

typedef struct UWND_ARM64_Inst UWND_ARM64_Inst;
struct UWND_ARM64_Inst
{
  UWND_ARM64_InstKind kind;
  S64 sp_delta;      // bytes added to sp by the instruction's writeback
  B32 touches_frame; // a load/store which includes x29 or x30
};

// rjf: an instruction outside the recognized prologue / epilogue forms, which
// can be shown to write none of sp, x29 and x30 and to continue at the next
// instruction (so the frame registers are the same before and after it)
internal B32
uwnd_arm64_code_is_frame_neutral(U32 code)
{
  B32 neutral = 0;
  U32 rd  = (code & 0x1f);
  U32 rn  = (code >> 5) & 0x1f;
  U32 rt2 = (code >> 10) & 0x1f;
  B32 rd_is_fp_lr = (rd == 29 || rd == 30);
  
  //- rjf: loads & stores
  if((code & 0x0a000000) == 0x08000000)
  {
    // rjf: register pairs
    if((code & 0x3a000000) == 0x28000000)
    {
      B32 is_load   = !!(code & (1u<<22));
      B32 is_vector = !!(code & (1u<<26));
      U32 addressing = (code >> 23) & 3;
      B32 writeback = (addressing == 1 || addressing == 3);
      neutral = (!(is_load && !is_vector && (rd == 29 || rd == 30 || rt2 == 29 || rt2 == 30)) && !(writeback && rn >= 29));
    }
    // rjf: single registers (unsigned offset, register offset, unscaled, pre/post-indexed)
    else if((code & 0x3b000000) == 0x38000000 || (code & 0x3b000000) == 0x39000000)
    {
      B32 is_vector = !!(code & (1u<<26));
      B32 writes_rt = (!is_vector && ((code >> 22) & 3) != 0); // opc 00: store; 01, 10, 11: loads (sign-extending included)
      B32 writeback = ((code & 0x3b200400) == 0x38000400);
      neutral = (!(writes_rt && rd_is_fp_lr) && !(writeback && rn >= 29));
    }
    // rjf: literal loads
    else if((code & 0x3b000000) == 0x18000000)
    {
      B32 is_vector = !!(code & (1u<<26));
      neutral = (is_vector || !rd_is_fp_lr);
    }
    // rjf: exclusives, atomics, structure loads/stores: not decoded
  }
  
  //- rjf: data processing (immediate): rd == 31 is sp for add/sub (not setting
  // flags) and for and/orr/eor; xzr otherwise
  else if((code & 0x1c000000) == 0x10000000)
  {
    U32 op = (code >> 23) & 7; // 00x: adr/adrp, 010: add/sub, 011: add/sub with tags, 100: logical, 101: move wide, 110: bitfield, 111: extract
    B32 sets_flags = !!(code & (1u<<29));
    B32 rd31_is_sp = (((op == 2 || op == 3) && !sets_flags) || (op == 4 && ((code >> 29) & 3) != 3));
    neutral = (!rd_is_fp_lr && !(rd == 31 && rd31_is_sp));
  }
  
  //- rjf: data processing (register): rd == 31 is sp only for add/sub (extended
  // register) not setting flags
  else if((code & 0x0e000000) == 0x0a000000)
  {
    B32 is_addsub_ext = ((code & 0x1fe00000) == 0x0b200000);
    B32 sets_flags = !!(code & (1u<<29));
    neutral = (!rd_is_fp_lr && !(rd == 31 && is_addsub_ext && !sets_flags));
  }
  
  //- rjf: data processing (SIMD & FP): writes a general register only through rd
  else if((code & 0x0e000000) == 0x0e000000)
  {
    neutral = !rd_is_fp_lr;
  }
  
  //- rjf: system: svc, barriers, mrs/msr/sys (branches, calls and traps are not neutral)
  else if((code & 0xffe0001f) == 0xd4000001 || (code & 0xfffff000) == 0xd5033000)
  {
    neutral = 1;
  }
  else if((code & 0xffd00000) == 0xd5100000)
  {
    neutral = !rd_is_fp_lr;
  }
  return neutral;
}

internal UWND_ARM64_Inst
uwnd_arm64_inst_from_code(U32 code)
{
  UWND_ARM64_Inst inst = {UWND_ARM64_InstKind_Unknown};
  U32 rt  = (code & 0x1f);
  U32 rn  = (code >> 5) & 0x1f;
  U32 rt2 = (code >> 10) & 0x1f;
  
  //- rjf: hints: nop, bti, pacibsp, paciasp, autibsp, autiasp, ...
  if((code & 0xfffff01f) == 0xd503201f)
  {
    inst.kind = UWND_ARM64_InstKind_Nop;
  }
  
  //- rjf: ret, retaa, retab
  else if(code == 0xd65f03c0 || code == 0xd65f0bff || code == 0xd65f0fff)
  {
    inst.kind = UWND_ARM64_InstKind_Return;
  }
  
  //- rjf: b imm26, br xN
  else if((code & 0xfc000000) == 0x14000000 || (code & 0xfffffc1f) == 0xd61f0000)
  {
    inst.kind = UWND_ARM64_InstKind_Branch;
  }
  
  //- rjf: b.cond, cbz/cbnz, tbz/tbnz
  else if((code & 0xff000010) == 0x54000000 || (code & 0x7e000000) == 0x34000000 || (code & 0x7e000000) == 0x36000000)
  {
    inst.kind = UWND_ARM64_InstKind_CondBranch;
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
  // (bit 25 clear: with it set this is the logical shifted-register class, e.g. `mov xN, xM` = `orr xN, xzr, xM`)
  else if(rn == 31 && (code & 0x3a000000) == 0x28000000 && ((code >> 23) & 3) != 0)
  {
    U32 addressing = (code >> 23) & 3; // 1: post-index, 2: signed offset, 3: pre-index
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
  
  //- rjf: anything else which leaves sp, x29 and x30 alone
  else if(uwnd_arm64_code_is_frame_neutral(code))
  {
    inst.kind = UWND_ARM64_InstKind_Other;
  }
  return inst;
}

// rjf: an instruction which a prologue replay can step over: it keeps lr and
// fp, and its effect on sp (if any) is known
internal B32
uwnd_arm64_inst_is_prologue(UWND_ARM64_Inst inst)
{
  return (inst.kind == UWND_ARM64_InstKind_SpSub ||
          inst.kind == UWND_ARM64_InstKind_SpAdd ||
          inst.kind == UWND_ARM64_InstKind_Store ||
          (inst.kind == UWND_ARM64_InstKind_Load && !inst.touches_frame) ||
          inst.kind == UWND_ARM64_InstKind_Nop ||
          inst.kind == UWND_ARM64_InstKind_Other);
}

internal UWND_ARM64_CodeState
uwnd_arm64_code_state_from_pc(MemoryMap *memory_map, U64 function_vaddr, U64 pc, U64 sp, U64 *cfa_out, Rng1U64 *missed_out)
{
  UWND_ARM64_CodeState state = UWND_ARM64_CodeState_Body;
  U64 cfa = 0;
  Rng1U64 missed = {0};
  if((pc & 3) == 0)
  {
    //////////////////////////////
    //- rjf: read the code around pc in one go: the 32 instructions behind it
    // (or the whole prologue, when the function start is known) and the 32
    // ahead, within pc's page (which is mapped: it is being executed). When
    // the memory map does not have it, the caller requests exactly this
    // range, and the analysis runs on the next attempt.
    //
    U64 context_bytes = 128;
    U64 page_vaddr = AlignDownPow2(pc, KB(4));
    B32 replay_prologue = (function_vaddr != 0 && function_vaddr <= pc && pc - function_vaddr <= context_bytes);
    Rng1U64 window = r1u64(Max(page_vaddr, pc - Min(pc, context_bytes)), Min(page_vaddr + KB(4), pc + context_bytes));
    if(replay_prologue)
    {
      window.min = Min(window.min, function_vaddr);
    }
    U32 code[64] = {0};
    if(memory_map_read(memory_map, window, code) != dim_1u64(window))
    {
      state = UWND_ARM64_CodeState_NeedRead;
      missed = window;
    }
    else
    {
      U64 behind_count = (pc - window.min)/4;    // code[0, behind_count) are the instructions behind pc
      U64 ahead_count = (window.max - pc)/4;     // pc_code[0, ahead_count) the ones from pc on
      U32 *pc_code = code + behind_count;
      
      //////////////////////////////
      //- rjf: prologue, known function start -> replay the instructions from
      // the start up to pc; if all of them are prologue instructions, and none
      // has set fp, the frame is not established yet
      //
      if(replay_prologue)
      {
        U32 *fn_code = code + (function_vaddr - window.min)/4;
        U64 count = (pc - function_vaddr)/4;
        S64 depth = 0;
        B32 in_prologue = 1;
        for EachIndex(idx, count)
        {
          UWND_ARM64_Inst inst = uwnd_arm64_inst_from_code(fn_code[idx]);
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
      
      //////////////////////////////
      //- rjf: prologue, unknown function start -> if a frame setup lies ahead
      // of pc, reachable only through prologue instructions, then pc is inside
      // a prologue; the prologue instructions immediately behind pc are the
      // ones which have already executed (the previous function ends with a
      // return, branch, or trap, which stops the backwards scan)
      //
      else if(function_vaddr == 0)
      {
        B32 setup_ahead = 0;
        for(U64 idx = 0; idx < ahead_count; idx += 1)
        {
          UWND_ARM64_Inst inst = uwnd_arm64_inst_from_code(pc_code[idx]);
          if(inst.kind == UWND_ARM64_InstKind_SetFp) { setup_ahead = 1; break; }
          if(!uwnd_arm64_inst_is_prologue(inst)) { break; }
        }
        if(setup_ahead)
        {
          S64 depth = 0;
          for(U64 idx = behind_count; idx > 0; idx -= 1)
          {
            UWND_ARM64_Inst inst = uwnd_arm64_inst_from_code(code[idx-1]);
            if(!uwnd_arm64_inst_is_prologue(inst)) { break; }
            depth -= inst.sp_delta;
          }
          state = UWND_ARM64_CodeState_Prologue;
          cfa = sp + depth;
        }
      }
      
      //////////////////////////////
      //- rjf: epilogue -> if a return (or a tail call, once the stack was
      // popped) lies ahead of pc, reachable without writing sp, x29 or x30
      // other than by stack pops, then the frame record has already been
      // restored (or was never stored: a frameless leaf), lr holds the return
      // address, and the remaining pops give the entry stack pointer.
      // Conditional branches fall through; a frame record pop or a stack pop
      // right behind pc also counts for a tail call at pc.
      //
      if(state == UWND_ARM64_CodeState_Body)
      {
        S64 remaining = 0;
        B32 popped_any = 0;
        if(behind_count > 0)
        {
          UWND_ARM64_Inst inst = uwnd_arm64_inst_from_code(code[behind_count-1]);
          popped_any = (inst.kind == UWND_ARM64_InstKind_SpAdd || (inst.kind == UWND_ARM64_InstKind_Load && inst.touches_frame && inst.sp_delta > 0));
        }
        for(U64 idx = 0; idx < ahead_count; idx += 1)
        {
          UWND_ARM64_Inst inst = uwnd_arm64_inst_from_code(pc_code[idx]);
          if(inst.kind == UWND_ARM64_InstKind_Return || (inst.kind == UWND_ARM64_InstKind_Branch && popped_any))
          {
            state = UWND_ARM64_CodeState_Epilogue;
            cfa = sp + remaining;
            break;
          }
          else if(inst.kind == UWND_ARM64_InstKind_Load && !inst.touches_frame && inst.sp_delta >= 0)
          {
            remaining += inst.sp_delta;
            popped_any = (popped_any || inst.sp_delta != 0);
          }
          else if(inst.kind == UWND_ARM64_InstKind_SpAdd)
          {
            remaining += inst.sp_delta;
            popped_any = 1;
          }
          else if(inst.kind == UWND_ARM64_InstKind_Store && inst.sp_delta == 0)
          {
            // rjf: a store into the frame: leaves the frame registers alone
          }
          else if(inst.kind != UWND_ARM64_InstKind_Nop && inst.kind != UWND_ARM64_InstKind_Other && inst.kind != UWND_ARM64_InstKind_CondBranch)
          {
            // rjf: a push, a frame record restore or setup, a branch before any pop, a call, a trap, or an unknown instruction
            break;
          }
        }
      }
    }
  }
  
  if(cfa_out != 0)
  {
    cfa_out[0] = cfa;
  }
  if(missed_out != 0)
  {
    missed_out[0] = missed;
  }
  return state;
}

#endif

////////////////////////////////
//~ rjf: Moduleless Unwinder

internal UWND_StepResult
uwnd_step_moduleless(Arch arch, MemoryMap *memory_map, UWND_ModuleInfo *module_info, void *regs)
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
    
    // rjf: analyze the code at pc, unless pc is an outer frame's return address
    // (its code has not run) or unreadable (a call through a bad pointer, most
    // likely: treat the stop as the callee's first instruction, like the
    // prologue state)
    U64 code_cfa = 0;
    Rng1U64 code_missed = {0};
    UWND_ARM64_CodeState code_state = UWND_ARM64_CodeState_Body;
    if(module_info != 0 && module_info->pc_code_unreadable)
    {
      code_state = UWND_ARM64_CodeState_Prologue;
      code_cfa = regs_arm64->sp;
    }
    else if(module_info == 0 || !module_info->pc_is_return_address)
    {
      code_state = uwnd_arm64_code_state_from_pc(memory_map, 0, regs_arm64->pc, regs_arm64->sp, &code_cfa, &code_missed);
    }
    if(code_state == UWND_ARM64_CodeState_NeedRead)
    {
      result.status = UWND_StepStatus_FailedMemoryRead;
      result.missed_read_vaddr_range = code_missed;
    }
    else if(code_state != UWND_ARM64_CodeState_Body)
    {
      regs_arm64->pc = regs_arm64->lr;
      regs_arm64->sp = code_cfa;
      result.status = UWND_StepStatus_Good;
    }
    
    // rjf: no frame record -> the outermost frame
    else if(fp == 0)
    {
      regs_arm64->pc = 0;
      result.status = UWND_StepStatus_Good;
    }
    else if((fp & 0xf) == 0)
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
    default:{result = uwnd_step_moduleless(arch, memory_map, module_info, regs);}break;
#define X(name, namespace_lower) case UWND_Unwinder_##name:{result = namespace_lower##_uwnd_step(arch, memory_map, module_info, tls_vaddr, regs, cfa_out);}break;
    UWND_Unwinder_XList
#undef X
  }
  return result;
}
