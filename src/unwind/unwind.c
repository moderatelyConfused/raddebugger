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
// has made so far, or those the epilogue has still to make. In a prologue no
// callee-saved register has been modified yet; in an epilogue the ones whose
// pops lie ahead of pc are still in their slots, the others already hold the
// caller's values.

#if defined(ARM64_H)

typedef enum UWND_ARM64_InstKind
{
  UWND_ARM64_InstKind_Unknown,    // not decoded: may write sp, x29 or x30, trap, or call; ends every analysis
  UWND_ARM64_InstKind_Other,      // writes none of sp, x29, x30, and continues at the next instruction (hints included)
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
  S64 sp_delta;            // bytes added to sp by the instruction's writeback
  B32 touches_frame;       // Load / Store: includes x29 or x30 (only consulted for loads)
  B32 writes_callee_saved; // writes one of x19-x28 or v8-v15 (an Other, or a Load of them): no compiler-generated prologue does so before the frame is set up, so the first such instruction ends a prologue replay
  U32 reg_count;           // Load / Store: the registers, 0-30: xN, 32-63: vN, ...
  U32 regs[2];
  U32 slot_size;           // ... the bytes per register slot, ...
  S64 slot_off;            // ... and the first register's slot, relative to sp before the instruction
};

#define uwnd_arm64_gpr_is_callee_saved(r) (19 <= (r) && (r) <= 28)
#define uwnd_arm64_vec_is_callee_saved(r) (8 <= (r) && (r) <= 15)

// rjf: an instruction outside the recognized prologue / epilogue forms, which
// can be shown to write none of sp, x29 and x30 and to continue at the next
// instruction (so the frame registers are the same before and after it); on
// the side, whether it writes a callee-saved register
internal B32
uwnd_arm64_code_is_frame_neutral(U32 code, B32 *writes_callee_saved_out)
{
  B32 neutral = 0;
  B32 writes_callee_saved = 0;
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
      writes_callee_saved = ((is_load && !is_vector && (uwnd_arm64_gpr_is_callee_saved(rd) || uwnd_arm64_gpr_is_callee_saved(rt2))) ||
                             (is_load && is_vector && (uwnd_arm64_vec_is_callee_saved(rd) || uwnd_arm64_vec_is_callee_saved(rt2))) ||
                             (writeback && uwnd_arm64_gpr_is_callee_saved(rn)));
    }
    // rjf: single registers (unsigned offset, register offset, unscaled, pre/post-indexed)
    else if((code & 0x3b000000) == 0x38000000 || (code & 0x3b000000) == 0x39000000)
    {
      B32 is_vector = !!(code & (1u<<26));
      B32 writes_rt = (!is_vector && ((code >> 22) & 3) != 0); // opc 00: store; 01, 10, 11: loads (sign-extending included)
      B32 writes_vt = (is_vector && !!(code & (1u<<22)));
      B32 writeback = ((code & 0x3b200400) == 0x38000400);
      neutral = (!(writes_rt && rd_is_fp_lr) && !(writeback && rn >= 29));
      writes_callee_saved = ((writes_rt && uwnd_arm64_gpr_is_callee_saved(rd)) ||
                             (writes_vt && uwnd_arm64_vec_is_callee_saved(rd)) ||
                             (writeback && uwnd_arm64_gpr_is_callee_saved(rn)));
    }
    // rjf: literal loads
    else if((code & 0x3b000000) == 0x18000000)
    {
      B32 is_vector = !!(code & (1u<<26));
      neutral = (is_vector || !rd_is_fp_lr);
      writes_callee_saved = (is_vector ? uwnd_arm64_vec_is_callee_saved(rd) : uwnd_arm64_gpr_is_callee_saved(rd));
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
    writes_callee_saved = uwnd_arm64_gpr_is_callee_saved(rd);
  }

  //- rjf: data processing (register): rd == 31 is sp only for add/sub (extended
  // register) not setting flags
  else if((code & 0x0e000000) == 0x0a000000)
  {
    B32 is_addsub_ext = ((code & 0x1fe00000) == 0x0b200000);
    B32 sets_flags = !!(code & (1u<<29));
    neutral = (!rd_is_fp_lr && !(rd == 31 && is_addsub_ext && !sets_flags));
    writes_callee_saved = uwnd_arm64_gpr_is_callee_saved(rd);
  }

  //- rjf: data processing (SIMD & FP): writes a general register only through
  // rd (fmov xN, dM; fcvt*), else a vector register rd
  else if((code & 0x0e000000) == 0x0e000000)
  {
    neutral = !rd_is_fp_lr;
    writes_callee_saved = (uwnd_arm64_gpr_is_callee_saved(rd) || uwnd_arm64_vec_is_callee_saved(rd));
  }

  //- rjf: system: hints (nop, bti, pointer authentication), svc, barriers,
  // mrs/msr/sys (branches, calls and traps are not neutral)
  else if((code & 0xfffff01f) == 0xd503201f || (code & 0xffe0001f) == 0xd4000001 || (code & 0xfffff000) == 0xd5033000)
  {
    neutral = 1;
  }
  else if((code & 0xffd00000) == 0xd5100000)
  {
    neutral = !rd_is_fp_lr;
    writes_callee_saved = (!!(code & (1u<<21)) && uwnd_arm64_gpr_is_callee_saved(rd)); // mrs writes rt; msr reads it
  }
  if(writes_callee_saved_out != 0)
  {
    writes_callee_saved_out[0] = writes_callee_saved;
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

  //- rjf: ret, retaa, retab
  if(code == 0xd65f03c0 || code == 0xd65f0bff || code == 0xd65f0fff)
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
    inst.reg_count     = 2;
    inst.regs[0]       = rt + (is_vector ? 32 : 0);
    inst.regs[1]       = rt2 + (is_vector ? 32 : 0);
    inst.slot_size     = (U32)scale;
    inst.slot_off      = (addressing == 1) ? 0 : imm7 * (S64)scale;
  }

  //- rjf: str/ldr {x,d}t, [sp, #imm] (unsigned offset)
  else if(rn == 31 && (code >> 30) == 3 && (code & 0x3b000000) == 0x39000000 && ((code >> 22) & 3) < 2)
  {
    U32 is_vector      = (code >> 26) & 1;
    inst.kind          = ((code >> 22) & 1) ? UWND_ARM64_InstKind_Load : UWND_ARM64_InstKind_Store;
    inst.touches_frame = (!is_vector && (rt == 29 || rt == 30));
    inst.reg_count     = 1;
    inst.regs[0]       = rt + (is_vector ? 32 : 0);
    inst.slot_size     = 8;
    inst.slot_off      = (S64)((code >> 10) & 0xfff) * 8;
  }

  //- rjf: str/ldr {x,d}t, [sp, #imm]! / [sp], #imm
  else if(rn == 31 && (code >> 30) == 3 && (code & 0x3b200400) == 0x38000400 && ((code >> 22) & 3) < 2)
  {
    U32 is_vector = (code >> 26) & 1;
    B32 pre_index = !!(code & (1u<<11));
    S64 imm9 = (S64)((code >> 12) & 0x1ff);
    if(imm9 & 0x100) { imm9 -= 0x200; }
    inst.kind          = ((code >> 22) & 1) ? UWND_ARM64_InstKind_Load : UWND_ARM64_InstKind_Store;
    inst.sp_delta      = imm9;
    inst.touches_frame = (!is_vector && (rt == 29 || rt == 30));
    inst.reg_count     = 1;
    inst.regs[0]       = rt + (is_vector ? 32 : 0);
    inst.slot_size     = 8;
    inst.slot_off      = pre_index ? imm9 : 0;
  }

  //- rjf: anything else which leaves sp, x29 and x30 alone
  else if(uwnd_arm64_code_is_frame_neutral(code, &inst.writes_callee_saved))
  {
    inst.kind = UWND_ARM64_InstKind_Other;
  }

  //- rjf: a load of a callee-saved register from the stack is its restore (an epilogue)
  if(inst.kind == UWND_ARM64_InstKind_Load)
  {
    for EachIndex(idx, inst.reg_count)
    {
      U32 reg = inst.regs[idx];
      if((reg < 32 && uwnd_arm64_gpr_is_callee_saved(reg)) || (reg >= 32 && uwnd_arm64_vec_is_callee_saved(reg - 32)))
      {
        inst.writes_callee_saved = 1;
      }
    }
  }
  return inst;
}

// rjf: an instruction which a prologue replay can step over: it keeps lr, fp
// and the callee-saved registers, its effect on sp (if any) is known, and it
// continues at the next instruction. A conditional branch falls through: pc
// is only reachable from the function start by falling through the branches
// before it (a forward target skips past pc), and a loop ahead of the frame
// setup (a shrink-wrapped function's early-exit test) contains no pushes in
// compiler output. A write of a callee-saved register means the saves are
// done: the frame is set up.
internal B32
uwnd_arm64_inst_is_prologue(UWND_ARM64_Inst inst)
{
  return (!inst.writes_callee_saved &&
          (inst.kind == UWND_ARM64_InstKind_SpSub ||
           inst.kind == UWND_ARM64_InstKind_SpAdd ||
           inst.kind == UWND_ARM64_InstKind_Store ||
           (inst.kind == UWND_ARM64_InstKind_Load && !inst.touches_frame) ||
           inst.kind == UWND_ARM64_InstKind_Other ||
           inst.kind == UWND_ARM64_InstKind_CondBranch));
}

// rjf: the instruction count of a linker stub starting at code[0], or 0 if it
// is not one: `adrp x16, page; ldr x16, [x16, #off]; br x16` (__stubs), or the
// arm64e form `adrp x17, page; add x17, x17, #off; ldr x16, [x17]; braa x16, x17`
// (__auth_stubs). Stubs have no symbol and no unwind entry, and never touch
// the stack: at any of their instructions lr holds the return address.
internal U64
uwnd_arm64_stub_inst_count_from_code(U32 *code, U64 count)
{
  U64 result = 0;
  if(count >= 3 &&
     (code[0] & 0x9f00001f) == 0x90000010 &&
     (code[1] & 0xffc003ff) == 0xf9400210 &&
     code[2] == 0xd61f0200)
  {
    result = 3;
  }
  else if(count >= 4 &&
          (code[0] & 0x9f00001f) == 0x90000011 &&
          (code[1] & 0xff8003ff) == 0x91000231 &&
          code[2] == 0xf9400230 &&
          code[3] == 0xd71f0a11)
  {
    result = 4;
  }
  return result;
}

internal UWND_ARM64_Analysis
uwnd_arm64_analysis_from_pc(MemoryMap *memory_map, U64 function_vaddr, U64 pc, U64 sp)
{
  UWND_ARM64_Analysis analysis = {UWND_ARM64_CodeState_Body};
  if((pc & 3) != 0)
  {
    // rjf: not an instruction address
  }

  //////////////////////////////
  //- rjf: at the function's first instruction nothing has run yet: lr holds
  // the return address and sp is the entry stack pointer, without a read (a
  // step into a call stops here)
  //
  else if(function_vaddr != 0 && pc == function_vaddr)
  {
    analysis.state = UWND_ARM64_CodeState_Prologue;
    analysis.cfa = sp;
  }
  else
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
      analysis.state = UWND_ARM64_CodeState_NeedRead;
      analysis.missed = window;
    }
    else
    {
      U64 behind_count = (pc - window.min)/4;    // code[0, behind_count) are the instructions behind pc
      U64 ahead_count = (window.max - pc)/4;     // pc_code[0, ahead_count) the ones from pc on
      U32 *pc_code = code + behind_count;

      //////////////////////////////
      //- rjf: prologue, known function start -> replay the instructions from
      // the start up to pc; if all of them are prologue instructions, and none
      // has set fp or written a callee-saved register, the frame is not
      // established yet
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
          analysis.state = UWND_ARM64_CodeState_Prologue;
          analysis.cfa = sp + depth;
        }
      }

      //////////////////////////////
      //- rjf: prologue, unknown function start (or one too far from pc to
      // replay from, or one the replay ran off: an unwind entry's start is
      // that of a run of same-encoding functions) -> if a frame setup lies
      // ahead of pc, reachable only through prologue instructions, then pc is
      // inside a prologue; the prologue instructions immediately behind pc are
      // the ones which have already executed (the previous function ends with
      // a return, branch, or trap, which stops the backwards scan; so does a
      // conditional branch, as the code behind it may be a loop ahead of the
      // frame setup, entered from before it)
      //
      if(analysis.state == UWND_ARM64_CodeState_Body)
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
            if(!uwnd_arm64_inst_is_prologue(inst) || inst.kind == UWND_ARM64_InstKind_CondBranch) { break; }
            depth -= inst.sp_delta;
          }
          analysis.state = UWND_ARM64_CodeState_Prologue;
          analysis.cfa = sp + depth;
        }
      }

      //////////////////////////////
      //- rjf: linker stub -> pc is at one of the instructions of a stub, which
      // has not touched the stack: lr holds the return address, sp is the
      // entry stack pointer (a step into a call through __stubs lands here)
      //
      if(analysis.state == UWND_ARM64_CodeState_Body)
      {
        for(U64 back = 0; back < 4 && back <= behind_count; back += 1)
        {
          U64 inst_count = uwnd_arm64_stub_inst_count_from_code(pc_code - back, ahead_count + back);
          if(inst_count != 0 && back < inst_count)
          {
            analysis.state = UWND_ARM64_CodeState_Prologue;
            analysis.cfa = sp;
            break;
          }
        }
      }

      //////////////////////////////
      //- rjf: epilogue -> if a return (or a tail call, once the stack was
      // popped) lies ahead of pc, reachable without writing sp, x29 or x30
      // other than by stack pops, then the frame record has already been
      // restored (or was never stored: a frameless leaf, or the early exit of
      // a shrink-wrapped function), lr holds the return address, and the
      // remaining pops give the entry stack pointer. The callee-saved
      // registers those pops restore are still in their slots; the others
      // already hold the caller's values (at a `ret` shared between an early
      // exit and the epilogue, the slots may never have been written).
      // Conditional branches fall through; a frame record pop or a stack pop
      // right behind pc also counts for a tail call at pc.
      //
      if(analysis.state == UWND_ARM64_CodeState_Body)
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
            analysis.state = UWND_ARM64_CodeState_Epilogue;
            analysis.cfa = sp + remaining;
            break;
          }
          else if(inst.kind == UWND_ARM64_InstKind_Load && !inst.touches_frame && inst.sp_delta >= 0)
          {
            // rjf: a pop, or a reload from the frame: the callee-saved registers it restores are pending
            for EachIndex(reg_idx, inst.reg_count)
            {
              U32 reg = inst.regs[reg_idx];
              B32 callee_saved = ((reg < 32 && uwnd_arm64_gpr_is_callee_saved(reg)) || (reg >= 32 && uwnd_arm64_vec_is_callee_saved(reg - 32)));
              if(callee_saved && inst.slot_size == 8 && analysis.pending_count < ArrayCount(analysis.pending))
              {
                analysis.pending[analysis.pending_count].reg = reg;
                analysis.pending[analysis.pending_count].vaddr = (U64)((S64)sp + remaining + inst.slot_off + (S64)reg_idx*8);
                analysis.pending_count += 1;
              }
            }
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
          else if(inst.kind != UWND_ARM64_InstKind_Other && inst.kind != UWND_ARM64_InstKind_CondBranch)
          {
            // rjf: a push, a frame record restore or setup, a branch before any pop, a call, a trap, or an unknown instruction
            break;
          }
        }
        if(analysis.state != UWND_ARM64_CodeState_Epilogue)
        {
          analysis.pending_count = 0;
        }
      }
    }
  }
  return analysis;
}

internal UWND_ARM64_CodeState
uwnd_arm64_code_state_from_pc(MemoryMap *memory_map, U64 function_vaddr, U64 pc, U64 sp, U64 *cfa_out, Rng1U64 *missed_out)
{
  UWND_ARM64_Analysis analysis = uwnd_arm64_analysis_from_pc(memory_map, function_vaddr, pc, sp);
  if(cfa_out != 0)
  {
    cfa_out[0] = analysis.cfa;
  }
  if(missed_out != 0)
  {
    missed_out[0] = analysis.missed;
  }
  return analysis.state;
}

// rjf: the analysis for a frame, per the module info: an outer frame's pc is
// a return address (its code has not run: Body), and a stop at code which
// cannot be read (a call through a bad pointer, most likely) is treated as
// the callee's first instruction, as lldb does
internal UWND_ARM64_Analysis
uwnd_arm64_analysis_from_module_info(MemoryMap *memory_map, UWND_ModuleInfo *module_info, U64 function_vaddr, ARM64_RegBlock *regs)
{
  UWND_ARM64_Analysis analysis = {UWND_ARM64_CodeState_Body};
  if(module_info != 0 && module_info->pc_code_unreadable)
  {
    analysis.state = UWND_ARM64_CodeState_Prologue;
    analysis.cfa = regs->sp;
  }
  else if(module_info == 0 || !module_info->pc_is_return_address)
  {
    analysis = uwnd_arm64_analysis_from_pc(memory_map, function_vaddr, regs->pc, regs->sp);
  }
  return analysis;
}

// rjf: Epilogue: restore the callee-saved registers whose pops lie ahead of pc
// from their slots (the registers are only written once every slot was read)
internal UWND_StepResult
uwnd_arm64_restore_pending(MemoryMap *memory_map, ARM64_RegBlock *regs, UWND_ARM64_Analysis *analysis)
{
  UWND_StepResult result = {UWND_StepStatus_Good};
  U64 values[ArrayCount(analysis->pending)] = {0};
  U64 count = Min(analysis->pending_count, ArrayCount(analysis->pending));
  Rng1U64 slots = {0};
  for EachIndex(idx, count)
  {
    U64 vaddr = analysis->pending[idx].vaddr;
    Rng1U64 slot = r1u64(vaddr, vaddr+8);
    slots = (idx == 0) ? slot : union_1u64(slots, slot);
    if(memory_map_read(memory_map, slot, &values[idx]) != 8)
    {
      result.status = UWND_StepStatus_FailedMemoryRead;
    }
  }
  if(result.status == UWND_StepStatus_FailedMemoryRead)
  {
    result.missed_read_vaddr_range = slots;
  }
  else
  {
    for EachIndex(idx, count)
    {
      U32 reg = analysis->pending[idx].reg;
      if(reg < 29)
      {
        (&regs->x0)[reg] = values[idx];
      }
      else if(32 <= reg && reg < 64)
      {
        (&regs->v0)[reg-32].u64[0] = values[idx];
      }
    }
  }
  return result;
}

// rjf: the frame pointer chain (the arm64 ABI requires frame pointers: [fp] =
// caller's fp, [fp+8] = return address), unless the analysis found pc in a
// prologue or epilogue, where the frame record is not in place
internal UWND_StepResult
uwnd_arm64_step_frame_chain(MemoryMap *memory_map, ARM64_RegBlock *regs, UWND_ARM64_Analysis *analysis)
{
  UWND_StepResult result = {UWND_StepStatus_Error};
  U64 fp = regs->fp;
  if(analysis->state == UWND_ARM64_CodeState_NeedRead)
  {
    result.status = UWND_StepStatus_FailedMemoryRead;
    result.missed_read_vaddr_range = analysis->missed;
  }
  else if(analysis->state != UWND_ARM64_CodeState_Body)
  {
    result = uwnd_arm64_restore_pending(memory_map, regs, analysis);
    if(result.status == UWND_StepStatus_Good)
    {
      regs->pc = regs->lr;
      regs->sp = analysis->cfa;
    }
  }

  // rjf: no frame record -> the outermost frame
  else if(fp == 0)
  {
    regs->pc = 0;
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
      regs->fp = frame[0];
      regs->lr = frame[1];
      regs->pc = frame[1];
      regs->sp = fp + 16;
      result.status = UWND_StepStatus_Good;
    }
  }
  return result;
}

#endif

////////////////////////////////
//~ rjf: Moduleless Unwinder

internal UWND_StepResult
uwnd_step_moduleless(Arch arch, MemoryMap *memory_map, UWND_ModuleInfo *module_info, void *regs)
{
  UWND_StepResult result = {UWND_StepStatus_Error};
#if defined(ARM64_H)
  //- rjf: arm64 -> the frame pointer chain, after the code analysis at pc
  if(arch == Arch_arm64)
  {
    ARM64_RegBlock *regs_arm64 = (ARM64_RegBlock *)regs;
    U64 function_vaddr = (module_info != 0) ? module_info->function_vaddr : 0;
    UWND_ARM64_Analysis analysis = uwnd_arm64_analysis_from_module_info(memory_map, module_info, function_vaddr, regs_arm64);
    result = uwnd_arm64_step_frame_chain(memory_map, regs_arm64, &analysis);
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
