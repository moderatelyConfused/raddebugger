// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

////////////////////////////////
//~ rjf: arm64 Prologue / Epilogue Analysis Checks
//
// Exercises the instruction decoder and the code state analysis of
// `unwind/unwind.c` (`uwnd_arm64_inst_from_code`,
// `uwnd_arm64_code_state_from_pc`) on hand-assembled sequences; the encodings
// were checked against clang's assembler (`otool -t`). Build & run from the
// repository root:
//
//   clang -I src -D_DARWIN_C_SOURCE -g -Wno-unused-function -Wno-unused-variable -Wno-writable-strings -Wno-initializer-overrides -o build/unwind_arm64_test src/mac/tests/unwind_arm64_test.c -lpthread -lm -ldl && build/unwind_arm64_test
//
// Exits with the number of failed checks.

#define BUILD_TITLE "arm64 Unwind Analysis Test"
#define BUILD_CONSOLE_INTERFACE 1

#include "base/base_inc.h"
#include "arm64/arm64_inc.h"
#include "arch/arch_inc.h"
#include "unwind/unwind.h"

#include "base/base_inc.c"
#include "arm64/arm64_inc.c"
#include "arch/arch_inc.c"

// rjf: the module unwinders which `uwnd_step` dispatches to are not under test
internal UWND_StepResult pe_x64_uwnd_step(Arch arch, MemoryMap *memory_map, UWND_ModuleInfo *module_info, U64 tls_vaddr, void *regs, U64 *cfa_out) { UWND_StepResult result = {UWND_StepStatus_Error}; return result; }
internal UWND_StepResult eh_uwnd_step(Arch arch, MemoryMap *memory_map, UWND_ModuleInfo *module_info, U64 tls_vaddr, void *regs, U64 *cfa_out)     { UWND_StepResult result = {UWND_StepStatus_Error}; return result; }
internal UWND_StepResult macho_uwnd_step(Arch arch, MemoryMap *memory_map, UWND_ModuleInfo *module_info, U64 tls_vaddr, void *regs, U64 *cfa_out)  { UWND_StepResult result = {UWND_StepStatus_Error}; return result; }

#include "unwind/unwind.c"

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

internal UWND_ARM64_CodeState
test_state(TestCode *tc, U64 function_vaddr, U64 pc, U64 sp, U64 *cfa_out)
{
  Rng1U64 missed = {0};
  return uwnd_arm64_code_state_from_pc(&tc->map, function_vaddr, pc, sp, cfa_out, &missed);
}

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
    inst = uwnd_arm64_inst_from_code(0xa9017bfd); // stp x29, x30, [sp, #16]
    Check(inst.kind == UWND_ARM64_InstKind_Store && inst.sp_delta == 0 && inst.touches_frame);
    inst = uwnd_arm64_inst_from_code(0xa8c17bfd); // ldp x29, x30, [sp], #16
    Check(inst.kind == UWND_ARM64_InstKind_Load && inst.sp_delta == 16 && inst.touches_frame);
    inst = uwnd_arm64_inst_from_code(0xa9417bfd); // ldp x29, x30, [sp, #16]
    Check(inst.kind == UWND_ARM64_InstKind_Load && inst.sp_delta == 0 && inst.touches_frame);
    inst = uwnd_arm64_inst_from_code(0xa94153f3); // ldp x19, x20, [sp, #16]
    Check(inst.kind == UWND_ARM64_InstKind_Load && inst.sp_delta == 0 && !inst.touches_frame);
    inst = uwnd_arm64_inst_from_code(0xf94007fe); // ldr x30, [sp, #8]
    Check(inst.kind == UWND_ARM64_InstKind_Load && inst.touches_frame);
    inst = uwnd_arm64_inst_from_code(0xf94037e8); // ldr x8, [sp, #0x68]
    Check(inst.kind == UWND_ARM64_InstKind_Load && inst.sp_delta == 0 && !inst.touches_frame);
  }

  //- rjf: stack adjustments, frame setup, returns, branches, hints
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
    Check(uwnd_arm64_inst_from_code(0xd503201f).kind == UWND_ARM64_InstKind_Nop);        // nop
    Check(uwnd_arm64_inst_from_code(0xd503245f).kind == UWND_ARM64_InstKind_Nop);        // bti c
    Check(uwnd_arm64_inst_from_code(0xd503233f).kind == UWND_ARM64_InstKind_Nop);        // paciasp
  }

  //- rjf: instructions which end an analysis: calls, traps, writes of sp / x29 / x30
  {
    Check(uwnd_arm64_inst_from_code(0x97ffffee).kind == UWND_ARM64_InstKind_Unknown); // bl
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

  //- rjf: instructions which leave the frame registers alone
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
    Check(test_state(tc, f, f+0x14, sp, &cfa) == UWND_ARM64_CodeState_Epilogue && cfa == sp);
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

////////////////////////////////
//~ rjf: Entry Point

internal void
entry_point(CmdLine *cmd_line)
{
  Arena *arena = arena_alloc();
  check_decoder();
  check_analysis(arena);
  fprintf(stderr, "%llu checks, %llu failed\n", (unsigned long long)check_count, (unsigned long long)check_fail_count);
  exit((int)check_fail_count);
}
