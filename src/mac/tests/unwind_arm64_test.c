// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

////////////////////////////////
//~ rjf: arm64 Prologue / Epilogue Analysis Checks
//
// Exercises the instruction decoder and the code state analysis of
// `unwind/unwind.c` (`uwnd_arm64_inst_from_code`,
// `uwnd_arm64_analysis_from_pc`) on hand-assembled sequences, the moduleless
// unwinder, and `macho/unwind/macho_unwind.c` on a hand-built `__unwind_info`
// (one frame and one frameless entry) with an `ARM64_RegBlock` and a stack
// page; the encodings were checked against clang's assembler (`otool -t`,
// `objdump -d`). Build & run from the repository root:
//
//   clang -I src -D_DARWIN_C_SOURCE -g -Wno-unused-function -Wno-unused-variable -Wno-writable-strings -Wno-initializer-overrides -o build/unwind_arm64_test src/mac/tests/unwind_arm64_test.c -lm -ldl && build/unwind_arm64_test
//
// Exits with the number of failed checks.

#define BUILD_TITLE "arm64 Unwind Analysis Test"
#define BUILD_CONSOLE_INTERFACE 1

#include "base/base_inc.h"
#include "arm64/arm64_inc.h"
#include "dwarf/dwarf_inc.h"
#include "eh_frame/eh_frame.h"
#include "arch/arch_inc.h"
#include "unwind/unwind.h"
#include "macho/unwind/macho_unwind.h"

#include "base/base_inc.c"
#include "arm64/arm64_inc.c"
#include "arch/arch_inc.c"

// rjf: the other module unwinders which `uwnd_step` dispatches to, and the DWARF CFI path of the Mach-O unwinder, are not under test
internal UWND_StepResult pe_x64_uwnd_step(Arch arch, MemoryMap *memory_map, UWND_ModuleInfo *module_info, U64 tls_vaddr, void *regs, U64 *cfa_out) { UWND_StepResult result = {UWND_StepStatus_Error}; return result; }
internal UWND_StepResult eh_uwnd_step(Arch arch, MemoryMap *memory_map, UWND_ModuleInfo *module_info, U64 tls_vaddr, void *regs, U64 *cfa_out)     { UWND_StepResult result = {UWND_StepStatus_Error}; return result; }
internal UWND_StepResult eh_uwnd_step_from_fde_vaddr(Arch arch, MemoryMap *memory_map, EH_PtrCtx *ptr_ctx, U64 fde_vaddr, U64 tls_vaddr, void *regs, B32 pc_is_return_address, U64 *cfa_out) { UWND_StepResult result = {UWND_StepStatus_Error}; return result; }

#include "unwind/unwind.c"
#include "macho/unwind/macho_unwind.c"

////////////////////////////////
//~ rjf: Check Helpers

global U64 check_count = 0;
global U64 check_fail_count = 0;

#define Check(c) do { check_count += 1; if(!(c)) { check_fail_count += 1; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while(0)

// rjf: two 4K pages of `udf #0` with `code` placed at `vaddr`, served through a memory map the way the engine fills it
typedef struct TestCode TestCode;
struct TestCode
{
  MemoryMap map;
  U64 page_vaddr;
  U8 page[KB(8)];
};

internal TestCode *
test_code(Arena *arena, U64 vaddr, U32 *code, U64 count)
{
  TestCode *tc = push_array(arena, TestCode, 1);
  tc->page_vaddr = AlignDownPow2(vaddr, KB(4));
  MemoryCopy(tc->page + (vaddr - tc->page_vaddr), code, count*sizeof(U32));
  memory_map_push(arena, &tc->map, r1u64(tc->page_vaddr, tc->page_vaddr + KB(8)), tc->page);
  return tc;
}

// rjf: more code into the same pages
internal void
test_code_add(TestCode *tc, U64 vaddr, U32 *code, U64 count)
{
  MemoryCopy(tc->page + (vaddr - tc->page_vaddr), code, count*sizeof(U32));
}

internal UWND_ARM64_CodeState
test_state(TestCode *tc, U64 function_vaddr, U64 pc, U64 sp, U64 *cfa_out)
{
  Rng1U64 missed = {0};
  return uwnd_arm64_code_state_from_pc(&tc->map, function_vaddr, pc, sp, cfa_out, &missed);
}

// rjf: is `reg` (19-28: xN, 40-47: vN) pending with slot `vaddr`?
internal B32
test_pending_has(UWND_ARM64_Analysis *a, U32 reg, U64 vaddr)
{
  B32 result = 0;
  for EachIndex(idx, a->pending_count)
  {
    if(a->pending[idx].reg == reg && a->pending[idx].vaddr == vaddr) { result = 1; }
  }
  return result;
}

// rjf: a 4K stack page served through the same memory map, with 8-byte slots relative to `base`
typedef struct TestStack TestStack;
struct TestStack
{
  U64 base;
  U8 page[KB(4)];
};

internal TestStack *
test_stack(Arena *arena, TestCode *tc, U64 base)
{
  TestStack *ts = push_array(arena, TestStack, 1);
  ts->base = base;
  memory_map_push(arena, &tc->map, r1u64(base, base + KB(4)), ts->page);
  return ts;
}

internal void
test_stack_set(TestStack *ts, U64 vaddr, U64 value)
{
  MemoryCopy(ts->page + (vaddr - ts->base), &value, 8);
}

// rjf: a `__unwind_info` section with one regular second level page holding `count` entries, functions ending at `end_voff`
internal MACHO_UWND_ModuleUnwindInfo *
test_unwind_info(Arena *arena, U32 *function_voffs, U32 *encodings, U64 count, U32 end_voff)
{
  U64 index_off   = sizeof(MACHO_UWND_SectionHeader);
  U64 page_off    = index_off + 2*sizeof(MACHO_UWND_IndexEntry);
  U64 entries_off = page_off + sizeof(MACHO_UWND_RegularPageHeader);
  U64 size        = entries_off + count*sizeof(MACHO_UWND_RegularEntry);
  U8 *data = push_array(arena, U8, size);
  MACHO_UWND_SectionHeader hdr = {MACHO_UWND_SECTION_VERSION, 0, 0, 0, 0, (U32)index_off, 2};
  MACHO_UWND_IndexEntry index[2] = {{function_voffs[0], (U32)page_off, 0}, {end_voff, 0, 0}};
  MACHO_UWND_RegularPageHeader page = {MACHO_UWND_SECOND_LEVEL_REGULAR, (U16)sizeof(MACHO_UWND_RegularPageHeader), (U16)count};
  MemoryCopy(data, &hdr, sizeof(hdr));
  MemoryCopy(data + index_off, index, sizeof(index));
  MemoryCopy(data + page_off, &page, sizeof(page));
  for EachIndex(idx, count)
  {
    MACHO_UWND_RegularEntry entry = {function_voffs[idx], encodings[idx]};
    MemoryCopy(data + entries_off + idx*sizeof(entry), &entry, sizeof(entry));
  }
  MACHO_UWND_ModuleUnwindInfo *info = push_array(arena, MACHO_UWND_ModuleUnwindInfo, 1);
  info->unwind_info_data = str8(data, size);
  return info;
}

internal UWND_StepResult
test_macho_step(TestCode *tc, MACHO_UWND_ModuleUnwindInfo *info, U64 base_vaddr, U64 function_vaddr, ARM64_RegBlock *regs, U64 *cfa_out)
{
  UWND_ModuleInfo module_info = {base_vaddr, info, 0, 0, function_vaddr};
  return macho_uwnd_step(Arch_arm64, &tc->map, &module_info, 0, regs, cfa_out);
}

internal UWND_StepResult
test_moduleless_step(TestCode *tc, U64 function_vaddr, ARM64_RegBlock *regs)
{
  UWND_ModuleInfo module_info = {0, 0, 0, 0, function_vaddr};
  return uwnd_step_moduleless(Arch_arm64, &tc->map, &module_info, regs);
}

////////////////////////////////
//~ rjf: Sequences
//
// `_f` and `_h` are Apple clang -O2's shrink-wrapped frame functions
// (`int f(int *p) { if(!p) return 0; return g(p)+1; }`, `int h(int n, int *p)
// { for(int i = 0; i < n; i++) { if(p[i] < 0) return i; } return g(p)*2; }`,
// compact encoding 0x04000000); `_f2` is `_f` saving x19/x20 (0x04000001);
// `leaf` is a frameless -O2 leaf saving x19-x22 (0x02002003).

// f+0x00 cbz x0, f+0x18 | stp x29, x30, [sp, #-16]! | mov x29, sp | bl g | add w0, w0, #1 | ldp x29, x30, [sp], #16 | f+0x18 ret
global U32 seq_f[] = {0xb40000c0, 0xa9bf7bfd, 0x910003fd, 0x94000000, 0x11000400, 0xa8c17bfd, 0xd65f03c0};

// h+0x00 cmp w0, #1 | b.lt h+0x28 | mov x8, x0 | mov x0, #0 | mov w8, w8 | h+0x14 ldr w9, [x1, x0, lsl #2] | tbnz w9, #31, h+0x40 | add x0, x0, #1 | cmp x8, x0 | b.ne h+0x14 |
// h+0x28 stp x29, x30, [sp, #-16]! | mov x29, sp | mov x0, x1 | bl g | lsl w0, w0, #1 | ldp x29, x30, [sp], #16 | h+0x40 ret
global U32 seq_h[] = {0x7100041f, 0x5400012b, 0xaa0003e8, 0xd2800000, 0x2a0803e8, 0xb8607829, 0x37f80149, 0x91000400, 0xeb00011f, 0x54ffff81,
  0xa9bf7bfd, 0x910003fd, 0xaa0103e0, 0x94000000, 0x531f7800, 0xa8c17bfd, 0xd65f03c0};

// f2+0x00 cbz x0, f2+0x24 | stp x20, x19, [sp, #-32]! | stp x29, x30, [sp, #16] | add x29, sp, #16 | f2+0x10 mov x19, x0 | bl g | add w0, w0, w19 |
// f2+0x1c ldp x29, x30, [sp, #16] | f2+0x20 ldp x20, x19, [sp], #32 | f2+0x24 ret
global U32 seq_f2[] = {0xb4000120, 0xa9be4ff4, 0xa9017bfd, 0x910043fd, 0xaa0003f3, 0x94000005, 0x0b130000, 0xa9417bfd, 0xa8c24ff4, 0xd65f03c0};

// +0x00 stp x22, x21, [sp, #-32]! | stp x20, x19, [sp, #16] | mov x16, x2 | mov x17, x1 | +0x10 mov x20, #0 | mov w19, #0 | adrp x21 | ldr x21, [x21] |
// +0x20 ldr w8, [x21, x20] | add w9, w19, w17 | madd w19, w8, w0, w9 | str w19, [x16, x20] | add x20, x20, #4 | cmp x20, #0x100 | b.ne +0x20 |
// +0x3c mov x0, x19 | +0x40 ldp x20, x19, [sp, #16] | +0x44 ldp x22, x21, [sp], #32 | +0x48 ret
global U32 seq_leaf[] = {0xa9be57f6, 0xa9014ff4, 0xaa0203f0, 0xaa0103f1, 0xd2800014, 0x52800013, 0x90000015, 0xf94002b5,
  0xb8746aa8, 0x0b110269, 0x1b002513, 0xb8346a13, 0x91001294, 0xf104029f, 0x54ffff41, 0xaa1303e0, 0xa9414ff4, 0xa8c257f6, 0xd65f03c0};

////////////////////////////////
//~ rjf: Checks

internal void
check_decoder(void)
{
  //- rjf: the finding: `mov x8, x1` (orr x8, xzr, x1) is a logical shifted-register op, not a pair store to [sp]
  {
    UWND_ARM64_Inst inst = uwnd_arm64_inst_from_code(0xaa0103e8);
    Check(inst.kind == UWND_ARM64_InstKind_Other);
    Check(inst.sp_delta == 0);
  }

  //- rjf: frame record stores / loads
  {
    UWND_ARM64_Inst inst = uwnd_arm64_inst_from_code(0xa9bf7bfd); // stp x29, x30, [sp, #-16]!
    Check(inst.kind == UWND_ARM64_InstKind_Store && inst.sp_delta == -16 && inst.touches_frame);
    Check(inst.reg_count == 2 && inst.regs[0] == 29 && inst.regs[1] == 30 && inst.slot_size == 8 && inst.slot_off == -16 && !inst.writes_callee_saved);
    inst = uwnd_arm64_inst_from_code(0xa9017bfd); // stp x29, x30, [sp, #16]
    Check(inst.kind == UWND_ARM64_InstKind_Store && inst.sp_delta == 0 && inst.touches_frame && inst.slot_off == 16);
    inst = uwnd_arm64_inst_from_code(0xa8c17bfd); // ldp x29, x30, [sp], #16
    Check(inst.kind == UWND_ARM64_InstKind_Load && inst.sp_delta == 16 && inst.touches_frame && inst.slot_off == 0 && !inst.writes_callee_saved);
    inst = uwnd_arm64_inst_from_code(0xa9417bfd); // ldp x29, x30, [sp, #16]
    Check(inst.kind == UWND_ARM64_InstKind_Load && inst.sp_delta == 0 && inst.touches_frame && inst.slot_off == 16);
    inst = uwnd_arm64_inst_from_code(0xa94153f3); // ldp x19, x20, [sp, #16]
    Check(inst.kind == UWND_ARM64_InstKind_Load && inst.sp_delta == 0 && !inst.touches_frame && inst.writes_callee_saved);
    Check(inst.reg_count == 2 && inst.regs[0] == 19 && inst.regs[1] == 20 && inst.slot_off == 16);
    inst = uwnd_arm64_inst_from_code(0xf94007fe); // ldr x30, [sp, #8]
    Check(inst.kind == UWND_ARM64_InstKind_Load && inst.touches_frame && inst.reg_count == 1 && inst.regs[0] == 30 && inst.slot_off == 8);
    inst = uwnd_arm64_inst_from_code(0xf94037e8); // ldr x8, [sp, #0x68]
    Check(inst.kind == UWND_ARM64_InstKind_Load && inst.sp_delta == 0 && !inst.touches_frame && !inst.writes_callee_saved && inst.slot_off == 0x68);
  }

  //- rjf: callee-saved register restores from the stack (loads of x19-x28 / d8-d15), and their slots
  {
    UWND_ARM64_Inst inst = uwnd_arm64_inst_from_code(0xa8c24ff4); // ldp x20, x19, [sp], #32
    Check(inst.kind == UWND_ARM64_InstKind_Load && inst.sp_delta == 32 && inst.writes_callee_saved && inst.regs[0] == 20 && inst.regs[1] == 19 && inst.slot_off == 0);
    inst = uwnd_arm64_inst_from_code(0xa9c153f3); // ldp x19, x20, [sp, #16]!
    Check(inst.kind == UWND_ARM64_InstKind_Load && inst.sp_delta == 16 && inst.slot_off == 16 && inst.writes_callee_saved);
    inst = uwnd_arm64_inst_from_code(0xf94007f3); // ldr x19, [sp, #8]
    Check(inst.kind == UWND_ARM64_InstKind_Load && inst.writes_callee_saved && inst.reg_count == 1 && inst.regs[0] == 19 && inst.slot_off == 8);
    inst = uwnd_arm64_inst_from_code(0x6d4127e8); // ldp d8, d9, [sp, #16]
    Check(inst.kind == UWND_ARM64_InstKind_Load && inst.sp_delta == 0 && inst.writes_callee_saved && inst.regs[0] == 32+8 && inst.regs[1] == 32+9 && inst.slot_size == 8 && inst.slot_off == 16);
    inst = uwnd_arm64_inst_from_code(0x6cc227e8); // ldp d8, d9, [sp], #32
    Check(inst.kind == UWND_ARM64_InstKind_Load && inst.sp_delta == 32 && inst.slot_off == 0 && inst.writes_callee_saved);
    inst = uwnd_arm64_inst_from_code(0xfd400be8); // ldr d8, [sp, #16]
    Check(inst.kind == UWND_ARM64_InstKind_Load && inst.writes_callee_saved && inst.regs[0] == 32+8 && inst.slot_off == 16);
    inst = uwnd_arm64_inst_from_code(0xb9400bf3); // ldr w19, [sp, #8] (a 32-bit load: not a slot restore, but a write of x19)
    Check(inst.kind == UWND_ARM64_InstKind_Other && inst.writes_callee_saved);
  }

  //- rjf: stack adjustments, frame setup, returns, branches, hints (hints are frame-neutral)
  {
    Check(uwnd_arm64_inst_from_code(0xd10083ff).kind == UWND_ARM64_InstKind_SpSub && uwnd_arm64_inst_from_code(0xd10083ff).sp_delta == -32); // sub sp, sp, #0x20
    Check(uwnd_arm64_inst_from_code(0x910083ff).kind == UWND_ARM64_InstKind_SpAdd && uwnd_arm64_inst_from_code(0x910083ff).sp_delta == +32); // add sp, sp, #0x20
    Check(uwnd_arm64_inst_from_code(0x910003fd).kind == UWND_ARM64_InstKind_SetFp);      // mov x29, sp
    Check(uwnd_arm64_inst_from_code(0x910043fd).kind == UWND_ARM64_InstKind_SetFp);      // add x29, sp, #0x10
    Check(uwnd_arm64_inst_from_code(0xd65f03c0).kind == UWND_ARM64_InstKind_Return);     // ret
    Check(uwnd_arm64_inst_from_code(0x17fffff1).kind == UWND_ARM64_InstKind_Branch);     // b
    Check(uwnd_arm64_inst_from_code(0xd61f0200).kind == UWND_ARM64_InstKind_Branch);     // br x16
    Check(uwnd_arm64_inst_from_code(0x54fffe81).kind == UWND_ARM64_InstKind_CondBranch); // b.ne
    Check(uwnd_arm64_inst_from_code(0xb4000071).kind == UWND_ARM64_InstKind_CondBranch); // cbz x17
    Check(uwnd_arm64_inst_from_code(0x3707fe68).kind == UWND_ARM64_InstKind_CondBranch); // tbnz w8, #0
    Check(uwnd_arm64_inst_from_code(0xd503201f).kind == UWND_ARM64_InstKind_Other);      // nop
    Check(uwnd_arm64_inst_from_code(0xd503245f).kind == UWND_ARM64_InstKind_Other);      // bti c
    Check(uwnd_arm64_inst_from_code(0xd503233f).kind == UWND_ARM64_InstKind_Other);      // paciasp
    Check(!uwnd_arm64_inst_from_code(0xd503201f).writes_callee_saved);
  }

  //- rjf: instructions which end an analysis: calls, traps, writes of sp / x29 / x30
  {
    Check(uwnd_arm64_inst_from_code(0x97ffffee).kind == UWND_ARM64_InstKind_Unknown); // bl
    Check(uwnd_arm64_inst_from_code(0x94000000).kind == UWND_ARM64_InstKind_Unknown); // bl (unrelocated)
    Check(uwnd_arm64_inst_from_code(0xd63f0100).kind == UWND_ARM64_InstKind_Unknown); // blr x8
    Check(uwnd_arm64_inst_from_code(0xd4200020).kind == UWND_ARM64_InstKind_Unknown); // brk #1
    Check(uwnd_arm64_inst_from_code(0x00000000).kind == UWND_ARM64_InstKind_Unknown); // udf #0
    Check(uwnd_arm64_inst_from_code(0xaa0803fd).kind == UWND_ARM64_InstKind_Unknown); // mov x29, x8
    Check(uwnd_arm64_inst_from_code(0xaa0003fe).kind == UWND_ARM64_InstKind_Unknown); // mov x30, x0
    Check(uwnd_arm64_inst_from_code(0x9100013f).kind == UWND_ARM64_InstKind_Unknown); // mov sp, x9
    Check(uwnd_arm64_inst_from_code(0x8b2863ff).kind == UWND_ARM64_InstKind_Unknown); // add sp, sp, x8
    Check(uwnd_arm64_inst_from_code(0xf84087a0).kind == UWND_ARM64_InstKind_Unknown); // ldr x0, [x29], #8
    Check(uwnd_arm64_inst_from_code(0x9e66001e).kind == UWND_ARM64_InstKind_Unknown); // fmov x30, d0
  }

  //- rjf: instructions which leave the frame registers alone, and the callee-saved registers
  {
    Check(uwnd_arm64_inst_from_code(0xf100051f).kind == UWND_ARM64_InstKind_Other); // cmp x8, #1 (subs xzr)
    Check(uwnd_arm64_inst_from_code(0xf240011f).kind == UWND_ARM64_InstKind_Other); // tst x8, #1 (ands xzr)
    Check(uwnd_arm64_inst_from_code(0xab2863e0).kind == UWND_ARM64_InstKind_Other); // adds x0, sp, x8
    Check(uwnd_arm64_inst_from_code(0xaa0a0128).kind == UWND_ARM64_InstKind_Other); // orr x8, x9, x10
    Check(uwnd_arm64_inst_from_code(0xb9000be0).kind == UWND_ARM64_InstKind_Other); // str w0, [sp, #8]
    Check(uwnd_arm64_inst_from_code(0xb9400be8).kind == UWND_ARM64_InstKind_Other); // ldr w8, [sp, #8]
    Check(uwnd_arm64_inst_from_code(0x90000000).kind == UWND_ARM64_InstKind_Other); // adrp x0
    Check(uwnd_arm64_inst_from_code(0xf8408500).kind == UWND_ARM64_InstKind_Other); // ldr x0, [x8], #8
    Check(uwnd_arm64_inst_from_code(0xf81603a8).kind == UWND_ARM64_InstKind_Other); // stur x8, [x29, #-0xa0]
    Check(uwnd_arm64_inst_from_code(0x39414108).kind == UWND_ARM64_InstKind_Other); // ldrb w8, [x8, #0x50]
    Check(uwnd_arm64_inst_from_code(0xd53bd071).kind == UWND_ARM64_InstKind_Other); // mrs x17, TPIDRRO_EL0
    Check(uwnd_arm64_inst_from_code(0x927df231).kind == UWND_ARM64_InstKind_Other); // and x17, x17, #-8
    Check(uwnd_arm64_inst_from_code(0xf8707a31).kind == UWND_ARM64_InstKind_Other); // ldr x17, [x17, x16, lsl #3]
    Check(uwnd_arm64_inst_from_code(0x8b100220).kind == UWND_ARM64_InstKind_Other); // add x0, x17, x16
    Check(uwnd_arm64_inst_from_code(0xd4001001).kind == UWND_ARM64_InstKind_Other); // svc #0x80
    Check(uwnd_arm64_inst_from_code(0x3dc003e0).kind == UWND_ARM64_InstKind_Other); // ldr q0, [sp]
    Check(uwnd_arm64_inst_from_code(0x3d800be0).kind == UWND_ARM64_InstKind_Other); // str q0, [sp, #0x20]
    Check(!uwnd_arm64_inst_from_code(0xf100051f).writes_callee_saved);
    Check(!uwnd_arm64_inst_from_code(0xb9400be8).writes_callee_saved);
    Check(!uwnd_arm64_inst_from_code(0xf8408500).writes_callee_saved);
    Check(!uwnd_arm64_inst_from_code(0xd53bd071).writes_callee_saved);
    Check(!uwnd_arm64_inst_from_code(0x3dc003e0).writes_callee_saved);
    Check(!uwnd_arm64_inst_from_code(0xaa1303e8).writes_callee_saved); // mov x8, x19 (a read)
  }

  //- rjf: writes of callee-saved registers (the end of a prologue replay)
  {
    U32 codes[] =
    {
      0x52800013, // mov w19, #0
      0xd2800014, // mov x20, #0
      0x1b002513, // madd w19, w8, w0, w9
      0xf9400113, // ldr x19, [x8]
      0xf8408668, // ldr x8, [x19], #8 (writeback)
      0xf8008680, // str x0, [x20], #8 (writeback)
      0xa9405915, // ldp x21, x22, [x8]
      0xfd400108, // ldr d8, [x8]
      0x9e670008, // fmov d8, x0
      0x9e660013, // fmov x19, d0
      0xd53bd073, // mrs x19, TPIDRRO_EL0
      0x58fffe13, // ldr x19, label
      0x90000013, // adrp x19
      0x91002273, // add x19, x19, #8
    };
    for EachElement(idx, codes)
    {
      UWND_ARM64_Inst inst = uwnd_arm64_inst_from_code(codes[idx]);
      Check(inst.kind == UWND_ARM64_InstKind_Other && inst.writes_callee_saved);
    }
  }
}

internal void
check_analysis(Arena *arena)
{
  U64 sp = 0x16fdff000;
  U64 cfa = 0;

  //- rjf: the finding's sequence: `sub sp, sp, #0x20; mov x8, x1; str x8, [sp]` at f, pc = f+8 -> prologue, cfa = sp+32
  // (the mov used to decode as a pair store with a +16 writeback)
  {
    U32 code[] = {0xd10083ff, 0xaa0103e8, 0xf90003e8};
    U64 f = 0x100000800;
    TestCode *tc = test_code(arena, f, code, ArrayCount(code));
    Check(test_state(tc, f, f+8, sp, &cfa) == UWND_ARM64_CodeState_Prologue);
    Check(cfa == sp + 32);
  }

  //- rjf: a clang -O0 frame function
  //
  //   f+0x00  sub sp, sp, #0x20
  //   f+0x04  stp x29, x30, [sp, #0x10]
  //   f+0x08  add x29, sp, #0x10
  //   f+0x0c  str w0, [sp, #8]
  //   f+0x10  bl g
  //   f+0x14  ldr w8, [sp, #8]
  //   f+0x18  add w0, w8, #1
  //   f+0x1c  ldp x29, x30, [sp, #0x10]
  //   f+0x20  add sp, sp, #0x20
  //   f+0x24  ret
  //
  {
    U32 code[] = {0xd10083ff, 0xa9017bfd, 0x910043fd, 0xb9000be0, 0x97ffffee, 0xb9400be8, 0x11000500, 0xa9417bfd, 0x910083ff, 0xd65f03c0};
    U64 f = 0x100000800;
    TestCode *tc = test_code(arena, f, code, ArrayCount(code));

    // rjf: known function start
    Check(test_state(tc, f, f+0x00, sp, &cfa) == UWND_ARM64_CodeState_Prologue && cfa == sp);
    Check(test_state(tc, f, f+0x04, sp, &cfa) == UWND_ARM64_CodeState_Prologue && cfa == sp + 32);
    Check(test_state(tc, f, f+0x08, sp, &cfa) == UWND_ARM64_CodeState_Prologue && cfa == sp + 32);
    Check(test_state(tc, f, f+0x0c, sp, &cfa) == UWND_ARM64_CodeState_Body);
    Check(test_state(tc, f, f+0x10, sp, &cfa) == UWND_ARM64_CodeState_Body);
    Check(test_state(tc, f, f+0x14, sp, &cfa) == UWND_ARM64_CodeState_Body);
    Check(test_state(tc, f, f+0x1c, sp, &cfa) == UWND_ARM64_CodeState_Body);
    Check(test_state(tc, f, f+0x20, sp, &cfa) == UWND_ARM64_CodeState_Epilogue && cfa == sp + 32);
    Check(test_state(tc, f, f+0x24, sp, &cfa) == UWND_ARM64_CodeState_Epilogue && cfa == sp);

    // rjf: unknown function start (the frame setup ahead of pc identifies the prologue)
    Check(test_state(tc, 0, f+0x04, sp, &cfa) == UWND_ARM64_CodeState_Prologue && cfa == sp + 32);
    Check(test_state(tc, 0, f+0x08, sp, &cfa) == UWND_ARM64_CodeState_Prologue && cfa == sp + 32);
    Check(test_state(tc, 0, f+0x0c, sp, &cfa) == UWND_ARM64_CodeState_Body);
    Check(test_state(tc, 0, f+0x20, sp, &cfa) == UWND_ARM64_CodeState_Epilogue && cfa == sp + 32);

    // rjf: a function straddling a page boundary: the window is clamped to pc's page, but always
    // includes the prologue from a known function start
    U64 f2 = 0x100001000 - 0x10;
    TestCode *tc2 = test_code(arena, f2, code, ArrayCount(code));
    Check(test_state(tc2, f2, f2+0x04, sp, &cfa) == UWND_ARM64_CodeState_Prologue && cfa == sp + 32);
    Check(test_state(tc2, f2, f2+0x14, sp, &cfa) == UWND_ARM64_CodeState_Body);
    Check(test_state(tc2, f2, f2+0x20, sp, &cfa) == UWND_ARM64_CodeState_Epilogue && cfa == sp + 32);
    {
      MemoryMap empty = {0};
      Rng1U64 missed = {0};
      Check(uwnd_arm64_code_state_from_pc(&empty, 0, f2+0x14, sp, &cfa, &missed) == UWND_ARM64_CodeState_NeedRead);
      Check(missed.min == 0x100001000 && missed.max == f2+0x14+128);
      Check(uwnd_arm64_code_state_from_pc(&empty, f2, f2+0x14, sp, &cfa, &missed) == UWND_ARM64_CodeState_NeedRead);
      Check(missed.min == f2 && missed.max == f2+0x14+128);
    }
  }

  //- rjf: at a function's first instruction (a step into a call) nothing has run: the answer needs no read
  {
    MemoryMap empty = {0};
    Rng1U64 missed = {0};
    U64 f = 0x100000800;
    Check(uwnd_arm64_code_state_from_pc(&empty, f, f, sp, &cfa, &missed) == UWND_ARM64_CodeState_Prologue && cfa == sp && dim_1u64(missed) == 0);
    Check(uwnd_arm64_code_state_from_pc(&empty, 0, f, sp, &cfa, &missed) == UWND_ARM64_CodeState_NeedRead);
  }

  //- rjf: shrink-wrapped frame functions (Apple clang -O2): the early-exit test runs before the frame is set up, so a conditional
  // branch does not end the prologue replay, nor the forward scan for the frame setup; the pushes behind pc give the depth
  {
    U64 f = 0x100000800;
    TestCode *tc = test_code(arena, f, seq_f, ArrayCount(seq_f));
    Check(test_state(tc, f, f+0x00, sp, &cfa) == UWND_ARM64_CodeState_Prologue && cfa == sp);
    Check(test_state(tc, f, f+0x04, sp, &cfa) == UWND_ARM64_CodeState_Prologue && cfa == sp);       // at the stp (cbz not taken)
    Check(test_state(tc, f, f+0x08, sp, &cfa) == UWND_ARM64_CodeState_Prologue && cfa == sp + 16);  // at mov x29, sp
    Check(test_state(tc, f, f+0x0c, sp, &cfa) == UWND_ARM64_CodeState_Body);
    Check(test_state(tc, f, f+0x10, sp, &cfa) == UWND_ARM64_CodeState_Body);
    Check(test_state(tc, f, f+0x14, sp, &cfa) == UWND_ARM64_CodeState_Body);
    Check(test_state(tc, f, f+0x18, sp, &cfa) == UWND_ARM64_CodeState_Epilogue && cfa == sp);       // the shared ret: early exit or epilogue
    Check(test_state(tc, 0, f+0x04, sp, &cfa) == UWND_ARM64_CodeState_Prologue && cfa == sp);
    Check(test_state(tc, 0, f+0x08, sp, &cfa) == UWND_ARM64_CodeState_Prologue && cfa == sp + 16);
    Check(test_state(tc, 0, f+0x18, sp, &cfa) == UWND_ARM64_CodeState_Epilogue && cfa == sp);

    U64 h = 0x100000900;
    TestCode *th = test_code(arena, h, seq_h, ArrayCount(seq_h));
    Check(test_state(th, h, h+0x14, sp, &cfa) == UWND_ARM64_CodeState_Prologue && cfa == sp);       // in the loop ahead of the frame setup
    Check(test_state(th, h, h+0x18, sp, &cfa) == UWND_ARM64_CodeState_Prologue && cfa == sp);
    Check(test_state(th, h, h+0x24, sp, &cfa) == UWND_ARM64_CodeState_Prologue && cfa == sp);       // the back edge
    Check(test_state(th, h, h+0x28, sp, &cfa) == UWND_ARM64_CodeState_Prologue && cfa == sp);       // at the stp
    Check(test_state(th, h, h+0x2c, sp, &cfa) == UWND_ARM64_CodeState_Prologue && cfa == sp + 16);  // at mov x29, sp
    Check(test_state(th, h, h+0x30, sp, &cfa) == UWND_ARM64_CodeState_Body);
    Check(test_state(th, h, h+0x38, sp, &cfa) == UWND_ARM64_CodeState_Body);
    Check(test_state(th, h, h+0x40, sp, &cfa) == UWND_ARM64_CodeState_Epilogue && cfa == sp);
    Check(test_state(th, 0, h+0x14, sp, &cfa) == UWND_ARM64_CodeState_Prologue && cfa == sp);       // unknown start: the setup ahead, nothing pushed behind
    Check(test_state(th, 0, h+0x24, sp, &cfa) == UWND_ARM64_CodeState_Prologue && cfa == sp);
    Check(test_state(th, 0, h+0x2c, sp, &cfa) == UWND_ARM64_CodeState_Prologue && cfa == sp + 16);
    Check(test_state(th, 0, h+0x30, sp, &cfa) == UWND_ARM64_CodeState_Body);
    Check(test_state(th, 0, h+0x40, sp, &cfa) == UWND_ARM64_CodeState_Epilogue && cfa == sp);
  }

  //- rjf: a shrink-wrapped frame function saving x19/x20: the write of x19 after the frame setup is a body; at the ret shared
  // between the early exit and the epilogue nothing is pending (the slots may never have been written), at the pop before
  // it x19/x20 are
  {
    U64 f = 0x100000800;
    TestCode *tc = test_code(arena, f, seq_f2, ArrayCount(seq_f2));
    Check(test_state(tc, f, f+0x04, sp, &cfa) == UWND_ARM64_CodeState_Prologue && cfa == sp);
    Check(test_state(tc, f, f+0x08, sp, &cfa) == UWND_ARM64_CodeState_Prologue && cfa == sp + 32);
    Check(test_state(tc, f, f+0x0c, sp, &cfa) == UWND_ARM64_CodeState_Prologue && cfa == sp + 32);
    Check(test_state(tc, f, f+0x10, sp, &cfa) == UWND_ARM64_CodeState_Body);
    Check(test_state(tc, f, f+0x18, sp, &cfa) == UWND_ARM64_CodeState_Body);
    Check(test_state(tc, f, f+0x1c, sp, &cfa) == UWND_ARM64_CodeState_Body);
    UWND_ARM64_Analysis a = uwnd_arm64_analysis_from_pc(&tc->map, f, f+0x20, sp);
    Check(a.state == UWND_ARM64_CodeState_Epilogue && a.cfa == sp + 32 && a.pending_count == 2);
    Check(test_pending_has(&a, 20, sp) && test_pending_has(&a, 19, sp + 8));
    a = uwnd_arm64_analysis_from_pc(&tc->map, f, f+0x24, sp);
    Check(a.state == UWND_ARM64_CodeState_Epilogue && a.cfa == sp && a.pending_count == 0);
    a = uwnd_arm64_analysis_from_pc(&tc->map, 0, f+0x20, sp);
    Check(a.state == UWND_ARM64_CodeState_Epilogue && a.cfa == sp + 32 && a.pending_count == 2);
  }

  //- rjf: a frameless -O2 leaf saving x19-x22 (compact encoding 0x02002003): the relaxed replay would take the whole body for a
  // prologue; the first write of a callee-saved register (`mov x20, #0`) ends it, so that the frameless rule restores them.
  // In the epilogue the registers whose pops lie ahead are pending, with their slots relative to the entry stack pointer
  {
    U64 f = 0x100000800;
    TestCode *tc = test_code(arena, f, seq_leaf, ArrayCount(seq_leaf));
    Check(test_state(tc, f, f+0x00, sp, &cfa) == UWND_ARM64_CodeState_Prologue && cfa == sp);
    Check(test_state(tc, f, f+0x04, sp, &cfa) == UWND_ARM64_CodeState_Prologue && cfa == sp + 32);
    Check(test_state(tc, f, f+0x08, sp, &cfa) == UWND_ARM64_CodeState_Prologue && cfa == sp + 32);
    Check(test_state(tc, f, f+0x10, sp, &cfa) == UWND_ARM64_CodeState_Prologue && cfa == sp + 32);  // at mov x20, #0: not yet written
    Check(test_state(tc, f, f+0x14, sp, &cfa) != UWND_ARM64_CodeState_Prologue);                    // after it: the restore must happen
    Check(test_state(tc, f, f+0x20, sp, &cfa) != UWND_ARM64_CodeState_Prologue);
    Check(test_state(tc, 0, f+0x20, sp, &cfa) != UWND_ARM64_CodeState_Prologue);
    UWND_ARM64_Analysis a = uwnd_arm64_analysis_from_pc(&tc->map, f, f+0x40, sp);
    Check(a.state == UWND_ARM64_CodeState_Epilogue && a.cfa == sp + 32 && a.pending_count == 4);
    Check(test_pending_has(&a, 19, sp + 24) && test_pending_has(&a, 20, sp + 16) && test_pending_has(&a, 21, sp + 8) && test_pending_has(&a, 22, sp));
    a = uwnd_arm64_analysis_from_pc(&tc->map, f, f+0x44, sp);
    Check(a.state == UWND_ARM64_CodeState_Epilogue && a.cfa == sp + 32 && a.pending_count == 2);
    Check(test_pending_has(&a, 21, sp + 8) && test_pending_has(&a, 22, sp));
    a = uwnd_arm64_analysis_from_pc(&tc->map, f, f+0x48, sp);
    Check(a.state == UWND_ARM64_CodeState_Epilogue && a.cfa == sp && a.pending_count == 0);
  }

  //- rjf: a frameless leaf which never stores the frame record (dyld's `_tlv_get_addr`): lr holds the return address
  //
  //   ldr x16, [x0, #8]; mrs x17, TPIDRRO_EL0; and x17, x17, #-8; ldr x17, [x17, x16, lsl #3]; cbz x17, 1f; ldr x16, [x0, #16]; add x0, x17, x16; 1: ret
  //
  {
    U32 code[] = {0xf9400410, 0xd53bd071, 0x927df231, 0xf8707a31, 0xb4000071, 0xf9400810, 0x8b100220, 0xd65f03c0};
    U64 f = 0x100000800;
    TestCode *tc = test_code(arena, f, code, ArrayCount(code));
    Check(test_state(tc, 0, f+0x00, sp, &cfa) != UWND_ARM64_CodeState_Body && cfa == sp);
    Check(test_state(tc, 0, f+0x14, sp, &cfa) == UWND_ARM64_CodeState_Epilogue && cfa == sp);
    Check(test_state(tc, f, f+0x14, sp, &cfa) != UWND_ARM64_CodeState_Body && cfa == sp); // with the start known, the replay (through the cbz) answers first
  }

  //- rjf: a tail call right after the frame record pop: `ldp x29, x30, [sp], #16; b g`
  {
    U32 code[] = {0xb9400be8, 0xa8c17bfd, 0x17fffff1};
    U64 f = 0x100000800;
    TestCode *tc = test_code(arena, f, code, ArrayCount(code));
    Check(test_state(tc, 0, f+0x04, sp, &cfa) == UWND_ARM64_CodeState_Body);
    Check(test_state(tc, 0, f+0x08, sp, &cfa) == UWND_ARM64_CodeState_Epilogue && cfa == sp);
  }

  //- rjf: a branch in a body (no pop before it) is not a tail call
  {
    U32 code[] = {0xb9400be8, 0x17fffff1};
    U64 f = 0x100000800;
    TestCode *tc = test_code(arena, f, code, ArrayCount(code));
    Check(test_state(tc, 0, f+0x04, sp, &cfa) == UWND_ARM64_CodeState_Body);
  }

  //- rjf: a syscall stub (libsystem_kernel's `write`: no compact unwind entry): `mov x16, #4; svc #0x80; b.lo 1f; b cerror; 1: ret`.
  // With the function start known (the RDI procedure), the replay finds nothing pushed at every instruction, the error
  // branch included (the conditional branch falls through), and lr is the return address; without it the `b cerror` ahead
  // of the `ret` hides the frameless leaf
  {
    U32 code[] = {0xd2800090, 0xd4001001, 0x54000043, 0x14000010, 0xd65f03c0};
    U64 f = 0x100000800;
    TestCode *tc = test_code(arena, f, code, ArrayCount(code));
    Check(test_state(tc, f, f+0x00, sp, &cfa) == UWND_ARM64_CodeState_Prologue && cfa == sp);
    Check(test_state(tc, f, f+0x04, sp, &cfa) == UWND_ARM64_CodeState_Prologue && cfa == sp);
    Check(test_state(tc, f, f+0x08, sp, &cfa) == UWND_ARM64_CodeState_Prologue && cfa == sp);
    Check(test_state(tc, f, f+0x0c, sp, &cfa) == UWND_ARM64_CodeState_Prologue && cfa == sp);
    Check(test_state(tc, f, f+0x10, sp, &cfa) == UWND_ARM64_CodeState_Epilogue && cfa == sp);
    Check(test_state(tc, 0, f+0x00, sp, &cfa) == UWND_ARM64_CodeState_Body);
  }

  //- rjf: linker stubs (`__stubs`: `adrp x16; ldr x16, [x16, #off]; br x16`, two of them in a row), no symbol and no unwind
  // entry, so the function start is unknown: at any of a stub's instructions lr holds the return address
  {
    U32 code[] = {0x90000030, 0xf9400210, 0xd61f0200, 0x90000030, 0xf9400610, 0xd61f0200};
    U64 f = 0x100000b8c;
    TestCode *tc = test_code(arena, f, code, ArrayCount(code));
    Check(uwnd_arm64_stub_inst_count_from_code(code, ArrayCount(code)) == 3);
    Check(uwnd_arm64_stub_inst_count_from_code(code+1, ArrayCount(code)-1) == 0);
    Check(uwnd_arm64_stub_inst_count_from_code(code, 2) == 0);
    Check(test_state(tc, 0, f+0x00, sp, &cfa) == UWND_ARM64_CodeState_Prologue && cfa == sp);
    Check(test_state(tc, 0, f+0x04, sp, &cfa) == UWND_ARM64_CodeState_Prologue && cfa == sp);
    Check(test_state(tc, 0, f+0x08, sp, &cfa) == UWND_ARM64_CodeState_Prologue && cfa == sp);
    Check(test_state(tc, 0, f+0x0c, sp, &cfa) == UWND_ARM64_CodeState_Prologue && cfa == sp);
    Check(test_state(tc, 0, f+0x10, sp, &cfa) == UWND_ARM64_CodeState_Prologue && cfa == sp);
    Check(test_state(tc, 0, f+0x14, sp, &cfa) == UWND_ARM64_CodeState_Prologue && cfa == sp);
  }

  //- rjf: the arm64e stub form (`__auth_stubs`: `adrp x17; add x17, x17, #off; ldr x16, [x17]; braa x16, x17`)
  {
    U32 code[] = {0x90000011, 0x91004231, 0xf9400230, 0xd71f0a11};
    U64 f = 0x100000b8c;
    TestCode *tc = test_code(arena, f, code, ArrayCount(code));
    Check(uwnd_arm64_stub_inst_count_from_code(code, ArrayCount(code)) == 4);
    Check(test_state(tc, 0, f+0x00, sp, &cfa) == UWND_ARM64_CodeState_Prologue && cfa == sp);
    Check(test_state(tc, 0, f+0x0c, sp, &cfa) == UWND_ARM64_CodeState_Prologue && cfa == sp);
  }

  //- rjf: an `adrp; ldr; br` through another register is not a stub (a jump table in a body)
  {
    U32 code[] = {0x90000029, 0xf9400129, 0xd61f0120};
    U64 f = 0x100000800;
    TestCode *tc = test_code(arena, f, code, ArrayCount(code));
    Check(uwnd_arm64_stub_inst_count_from_code(code, ArrayCount(code)) == 0);
    Check(test_state(tc, 0, f+0x00, sp, &cfa) == UWND_ARM64_CodeState_Body);
  }

  //- rjf: the function start the compact unwind entry gives is that of a run of same-encoding functions: `g` (ldr w8, [sp, #8];
  // add w0, w8, #1; nop; ret) precedes the -O0 frame function `f`, and the entry says both start at g. The replay from g runs off
  // at its `ret`; the frame setup ahead of pc then identifies f's prologue, and the pushes behind pc (up to g's `ret`) its depth
  {
    U32 code[] = {0xb9400be8, 0x11000500, 0xd503201f, 0xd65f03c0, 0xd10083ff, 0xa9017bfd, 0x910043fd, 0xb9000be0, 0x97ffffee, 0xb9400be8, 0x11000500, 0xa9417bfd, 0x910083ff, 0xd65f03c0};
    U64 g = 0x100000800;
    U64 f = g + 0x10;
    TestCode *tc = test_code(arena, g, code, ArrayCount(code));
    Check(test_state(tc, g, f+0x00, sp, &cfa) == UWND_ARM64_CodeState_Prologue && cfa == sp);
    Check(test_state(tc, g, f+0x04, sp, &cfa) == UWND_ARM64_CodeState_Prologue && cfa == sp + 32);
    Check(test_state(tc, g, f+0x08, sp, &cfa) == UWND_ARM64_CodeState_Prologue && cfa == sp + 32);
    Check(test_state(tc, g, f+0x0c, sp, &cfa) == UWND_ARM64_CodeState_Body);
    Check(test_state(tc, g, f+0x24, sp, &cfa) == UWND_ARM64_CodeState_Epilogue && cfa == sp);
    // rjf: the same with the run's start beyond the replay window (basic_O0's `locals_frame`, 252 bytes after its entry's offset)
    Check(test_state(tc, g - 0x100, f+0x00, sp, &cfa) == UWND_ARM64_CodeState_Prologue && cfa == sp);
    Check(test_state(tc, g - 0x100, f+0x04, sp, &cfa) == UWND_ARM64_CodeState_Prologue && cfa == sp + 32);
    Check(test_state(tc, g - 0x100, f+0x0c, sp, &cfa) == UWND_ARM64_CodeState_Body);
    // rjf: and with f's real start (debug info): the replay alone answers
    Check(test_state(tc, f, f+0x00, sp, &cfa) == UWND_ARM64_CodeState_Prologue && cfa == sp);
    Check(test_state(tc, f, f+0x04, sp, &cfa) == UWND_ARM64_CodeState_Prologue && cfa == sp + 32);
    Check(test_state(tc, f, f+0x0c, sp, &cfa) == UWND_ARM64_CodeState_Body);
  }

  //- rjf: code not in the memory map -> the analysis asks for the window around pc; once read, it answers
  {
    U32 code[] = {0xd10083ff, 0xaa0103e8, 0xf90003e8};
    U64 f = 0x100000800;
    TestCode *tc = test_code(arena, f, code, ArrayCount(code));
    MemoryMap empty = {0};
    Rng1U64 missed = {0};
    Check(uwnd_arm64_code_state_from_pc(&empty, f, f+8, sp, &cfa, &missed) == UWND_ARM64_CodeState_NeedRead);
    Check(contains_1u64(missed, f+8) && dim_1u64(missed) <= 256 && missed.min >= tc->page_vaddr && missed.max <= tc->page_vaddr + KB(4));
    memory_map_push(arena, &empty, missed, tc->page + (missed.min - tc->page_vaddr));
    Check(uwnd_arm64_code_state_from_pc(&empty, f, f+8, sp, &cfa, &missed) == UWND_ARM64_CodeState_Prologue && cfa == sp + 32);
  }
}

internal void
check_steps(Arena *arena)
{
  //- rjf: a module at 0x100000000 with `_f2` at voff 0x1000 (frame, x19/x20 saved) and `leaf` at voff 0x1100 (frameless,
  // 32 bytes, x19-x22 saved), and a stack page; S is the stack pointer after the 32-byte push of either function
  U64 base = 0x100000000;
  U64 f2 = base + 0x1000;
  U64 leaf = base + 0x1100;
  TestCode *tc = test_code(arena, f2, seq_f2, ArrayCount(seq_f2));
  test_code_add(tc, leaf, seq_leaf, ArrayCount(seq_leaf));
  U32 voffs[] = {0x1000, 0x1100};
  U32 encodings[] = {MACHO_UWND_ARM64_MODE_FRAME | MACHO_UWND_ARM64_FRAME_X19_X20_PAIR, 0x02002003};
  MACHO_UWND_ModuleUnwindInfo *info = test_unwind_info(arena, voffs, encodings, ArrayCount(voffs), 0x1200);
  U64 stack_base = 0x16fdfe000;
  TestStack *ts = test_stack(arena, tc, stack_base);
  U64 S = stack_base + 0x800;
  U64 ra = base + 0x2004;      // the return address (the caller's call is at 0x2000)
  U64 caller_fp = S + 0x100;
  U64 cfa = 0;

  //- rjf: the lookups
  {
    U32 encoding = 0;
    U64 function_voff = 0;
    Check(macho_uwnd_encoding_from_voff(info, 0x1000, &encoding, &function_voff) && encoding == encodings[0] && function_voff == 0x1000);
    Check(macho_uwnd_encoding_from_voff(info, 0x1024, &encoding, &function_voff) && encoding == encodings[0] && function_voff == 0x1000);
    Check(macho_uwnd_encoding_from_voff(info, 0x1148, &encoding, &function_voff) && encoding == encodings[1] && function_voff == 0x1100);
    Check(!macho_uwnd_encoding_from_voff(info, 0x0800, &encoding, &function_voff));
  }

  //- rjf: `_f2`, body (after the call): the frame rule, x19/x20 from below the frame record
  {
    ARM64_RegBlock r = {0};
    r.pc = f2 + 0x18; r.sp = S; r.fp = S + 16; r.lr = f2 + 0x18; r.x19 = 0xca11ee19; r.x20 = 0xca11ee20;
    test_stack_set(ts, S + 0, 0x20);          // x20
    test_stack_set(ts, S + 8, 0x19);          // x19
    test_stack_set(ts, S + 16, caller_fp);    // frame record
    test_stack_set(ts, S + 24, ra);
    UWND_StepResult step = test_macho_step(tc, info, base, f2, &r, &cfa);
    Check(step.status == UWND_StepStatus_Good);
    Check(r.pc == ra && r.fp == caller_fp && r.sp == S + 32 && cfa == S + 32);
    Check(r.x19 == 0x19 && r.x20 == 0x20);
  }

  //- rjf: `_f2`, prologue: at the second stp (x19/x20 pushed, unmodified), at the first stp (reached through the cbz), and at
  // the first instruction; lr is the return address and nothing is restored, also without the function start (the entry's)
  {
    ARM64_RegBlock r = {0};
    r.pc = f2 + 0x08; r.sp = S; r.fp = caller_fp; r.lr = ra; r.x19 = 0x19; r.x20 = 0x20;
    test_stack_set(ts, S + 0, 0xbad);
    test_stack_set(ts, S + 8, 0xbad);
    UWND_StepResult step = test_macho_step(tc, info, base, f2, &r, &cfa);
    Check(step.status == UWND_StepStatus_Good && r.pc == ra && r.fp == caller_fp && r.sp == S + 32 && cfa == S + 32 && r.x19 == 0x19 && r.x20 == 0x20);
    r.pc = f2 + 0x04; r.sp = S + 32; r.fp = caller_fp; r.lr = ra;
    step = test_macho_step(tc, info, base, f2, &r, &cfa);
    Check(step.status == UWND_StepStatus_Good && r.pc == ra && r.fp == caller_fp && r.sp == S + 32 && cfa == S + 32 && r.x19 == 0x19);
    r.pc = f2 + 0x00; r.sp = S + 32; r.fp = caller_fp; r.lr = ra;
    step = test_macho_step(tc, info, base, f2, &r, &cfa);
    Check(step.status == UWND_StepStatus_Good && r.pc == ra && r.fp == caller_fp && r.sp == S + 32 && cfa == S + 32);
    r.pc = f2 + 0x08; r.sp = S; r.fp = caller_fp; r.lr = ra;
    step = test_macho_step(tc, info, base, 0, &r, &cfa);
    Check(step.status == UWND_StepStatus_Good && r.pc == ra && r.fp == caller_fp && r.sp == S + 32 && r.x19 == 0x19);
  }

  //- rjf: `_f2`, epilogue after the frame record pop: x19/x20 still hold the callee's values and are restored from their slots
  {
    ARM64_RegBlock r = {0};
    r.pc = f2 + 0x20; r.sp = S; r.fp = caller_fp; r.lr = ra; r.x19 = 0xca11ee19; r.x20 = 0xca11ee20;
    test_stack_set(ts, S + 0, 0x20);
    test_stack_set(ts, S + 8, 0x19);
    UWND_StepResult step = test_macho_step(tc, info, base, f2, &r, &cfa);
    Check(step.status == UWND_StepStatus_Good && r.pc == ra && r.fp == caller_fp && r.sp == S + 32 && cfa == S + 32);
    Check(r.x19 == 0x19 && r.x20 == 0x20);
    // rjf: the slots not in the memory map: the read is requested, and the registers are untouched
    MemoryMap code_only = {0};
    memory_map_push(arena, &code_only, r1u64(tc->page_vaddr, tc->page_vaddr + KB(8)), tc->page);
    r.pc = f2 + 0x20; r.sp = S; r.fp = caller_fp; r.lr = ra; r.x19 = 0xca11ee19; r.x20 = 0xca11ee20;
    UWND_ModuleInfo module_info = {base, info, 0, 0, f2};
    step = macho_uwnd_step(Arch_arm64, &code_only, &module_info, 0, &r, &cfa);
    Check(step.status == UWND_StepStatus_FailedMemoryRead && step.missed_read_vaddr_range.min == S && step.missed_read_vaddr_range.max == S + 16);
    Check(r.pc == f2 + 0x20 && r.x19 == 0xca11ee19);
  }

  //- rjf: `_f2`, the ret shared between the early exit and the epilogue: x19/x20 hold the caller's values either way, and
  // the slots may never have been written (the finding: the frame rule's layout read them)
  {
    ARM64_RegBlock r = {0};
    r.pc = f2 + 0x24; r.sp = S + 32; r.fp = caller_fp; r.lr = ra; r.x19 = 0x19; r.x20 = 0x20;
    test_stack_set(ts, S + 0, 0xbad);
    test_stack_set(ts, S + 8, 0xbad);
    UWND_StepResult step = test_macho_step(tc, info, base, f2, &r, &cfa);
    Check(step.status == UWND_StepStatus_Good && r.pc == ra && r.fp == caller_fp && r.sp == S + 32 && cfa == S + 32);
    Check(r.x19 == 0x19 && r.x20 == 0x20);
  }

  //- rjf: `_f2`, body with fp == 0: the outermost frame (pc 0, a good step)
  {
    ARM64_RegBlock r = {0};
    r.pc = f2 + 0x18; r.sp = S; r.fp = 0; r.lr = f2 + 0x18;
    UWND_StepResult step = test_macho_step(tc, info, base, f2, &r, &cfa);
    Check(step.status == UWND_StepStatus_Good && r.pc == 0);
    r.pc = f2 + 0x18; r.sp = S; r.fp = 0;
    step = test_moduleless_step(tc, f2, &r);
    Check(step.status == UWND_StepStatus_Good && r.pc == 0);
  }

  //- rjf: `leaf`, prologue (after the pushes, before `mov x20, #0`): the registers hold the caller's values, nothing restored
  {
    ARM64_RegBlock r = {0};
    r.pc = leaf + 0x08; r.sp = S; r.fp = caller_fp; r.lr = ra; r.x19 = 0x19; r.x20 = 0x20; r.x21 = 0x21; r.x22 = 0x22;
    test_stack_set(ts, S + 0, 0xbad); test_stack_set(ts, S + 8, 0xbad); test_stack_set(ts, S + 16, 0xbad); test_stack_set(ts, S + 24, 0xbad);
    UWND_StepResult step = test_macho_step(tc, info, base, leaf, &r, &cfa);
    Check(step.status == UWND_StepStatus_Good && r.pc == ra && r.fp == caller_fp && r.sp == S + 32 && cfa == S + 32);
    Check(r.x19 == 0x19 && r.x20 == 0x20 && r.x21 == 0x21 && r.x22 == 0x22);
  }

  //- rjf: `leaf`, in the loop (x19-x22 modified): all four come back from their slots (the finding: the whole body read as
  // a prologue, and the caller's registers kept the callee's values)
  {
    ARM64_RegBlock r = {0};
    r.pc = leaf + 0x20; r.sp = S; r.fp = caller_fp; r.lr = ra; r.x19 = 0xca11ee19; r.x20 = 0xca11ee20; r.x21 = 0xca11ee21; r.x22 = 0xca11ee22;
    test_stack_set(ts, S + 0, 0x22); test_stack_set(ts, S + 8, 0x21); test_stack_set(ts, S + 16, 0x20); test_stack_set(ts, S + 24, 0x19);
    UWND_StepResult step = test_macho_step(tc, info, base, leaf, &r, &cfa);
    Check(step.status == UWND_StepStatus_Good && r.pc == ra && r.fp == caller_fp && r.sp == S + 32 && cfa == S + 32);
    Check(r.x19 == 0x19 && r.x20 == 0x20 && r.x21 == 0x21 && r.x22 == 0x22);
    // rjf: the same without the function start (the entry's), and at the instruction after the loop
    r.pc = leaf + 0x20; r.sp = S; r.x19 = 0xca11ee19; r.x20 = 0xca11ee20; r.x21 = 0xca11ee21; r.x22 = 0xca11ee22;
    step = test_macho_step(tc, info, base, 0, &r, &cfa);
    Check(step.status == UWND_StepStatus_Good && r.pc == ra && r.sp == S + 32 && r.x19 == 0x19 && r.x20 == 0x20 && r.x21 == 0x21 && r.x22 == 0x22);
    r.pc = leaf + 0x3c; r.sp = S; r.x19 = 0xca11ee19; r.x20 = 0xca11ee20; r.x21 = 0xca11ee21; r.x22 = 0xca11ee22;
    step = test_macho_step(tc, info, base, leaf, &r, &cfa);
    Check(step.status == UWND_StepStatus_Good && r.pc == ra && r.sp == S + 32 && r.x19 == 0x19 && r.x20 == 0x20 && r.x21 == 0x21 && r.x22 == 0x22);
  }

  //- rjf: `leaf`, epilogue after the first ldp: x19/x20 are back already, x21/x22 come from cfa-24 / cfa-32; at the ret
  // everything is in place. The moduleless unwinder (no unwind info for the function) restores the pending ones too
  {
    ARM64_RegBlock r = {0};
    r.pc = leaf + 0x44; r.sp = S; r.fp = caller_fp; r.lr = ra; r.x19 = 0x19; r.x20 = 0x20; r.x21 = 0xca11ee21; r.x22 = 0xca11ee22;
    test_stack_set(ts, S + 0, 0x22); test_stack_set(ts, S + 8, 0x21); test_stack_set(ts, S + 16, 0xbad); test_stack_set(ts, S + 24, 0xbad);
    UWND_StepResult step = test_macho_step(tc, info, base, leaf, &r, &cfa);
    Check(step.status == UWND_StepStatus_Good && r.pc == ra && r.fp == caller_fp && r.sp == S + 32 && cfa == S + 32);
    Check(r.x19 == 0x19 && r.x20 == 0x20 && r.x21 == 0x21 && r.x22 == 0x22);
    r.pc = leaf + 0x48; r.sp = S + 32; r.fp = caller_fp; r.lr = ra; r.x19 = 0x19; r.x20 = 0x20; r.x21 = 0x21; r.x22 = 0x22;
    test_stack_set(ts, S + 0, 0xbad); test_stack_set(ts, S + 8, 0xbad);
    step = test_macho_step(tc, info, base, leaf, &r, &cfa);
    Check(step.status == UWND_StepStatus_Good && r.pc == ra && r.fp == caller_fp && r.sp == S + 32 && cfa == S + 32);
    Check(r.x19 == 0x19 && r.x20 == 0x20 && r.x21 == 0x21 && r.x22 == 0x22);
    r.pc = leaf + 0x44; r.sp = S; r.fp = caller_fp; r.lr = ra; r.x21 = 0xca11ee21; r.x22 = 0xca11ee22;
    test_stack_set(ts, S + 0, 0x22); test_stack_set(ts, S + 8, 0x21);
    step = test_moduleless_step(tc, leaf, &r);
    Check(step.status == UWND_StepStatus_Good && r.pc == ra && r.fp == caller_fp && r.sp == S + 32 && r.x21 == 0x21 && r.x22 == 0x22);
  }

  //- rjf: an outer frame (pc is a return address): no code analysis, the frame rule at the call's address. The return address
  // of a call at the last instruction of `_f2`'s run (voff 0x1200) belongs to no entry, the call does
  {
    ARM64_RegBlock r = {0};
    r.pc = f2 + 0x18; r.sp = S; r.fp = S + 16; r.lr = 0; r.x19 = 0xca11ee19; r.x20 = 0xca11ee20;
    test_stack_set(ts, S + 0, 0x20); test_stack_set(ts, S + 8, 0x19); test_stack_set(ts, S + 16, caller_fp); test_stack_set(ts, S + 24, ra);
    UWND_ModuleInfo module_info = {base, info, 1, 0, 0};
    UWND_StepResult step = macho_uwnd_step(Arch_arm64, &tc->map, &module_info, 0, &r, &cfa);
    Check(step.status == UWND_StepStatus_Good && r.pc == ra && r.fp == caller_fp && r.sp == S + 32 && r.x19 == 0x19 && r.x20 == 0x20);
    U32 encoding = 0;
    U64 function_voff = 0;
    Check(macho_uwnd_encoding_from_voff(info, 0x1200 - 1, &encoding, &function_voff) && function_voff == 0x1100);
  }
}

////////////////////////////////
//~ rjf: Entry Point

internal void
entry_point(CmdLine *cmd_line)
{
  Arena *arena = arena_alloc();
  check_decoder();
  check_analysis(arena);
  check_steps(arena);
  fprintf(stderr, "%llu checks, %llu failed\n", (unsigned long long)check_count, (unsigned long long)check_fail_count);
  exit((int)check_fail_count);
}
