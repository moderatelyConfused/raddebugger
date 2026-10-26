// flows debuggee (F5c, F5d): stack pointer moves which are not context switches, and context
// switches between fiber stacks lying close to each other (-O2 only).
//
// Modes (argv[1]):
//   "vla"     (default) one line allocates a 512 KB VLA and then switches on a value through a
//             jump table (`br`): a step over the line must end on the next line (F5c)
//   "fibers"  two 64 KB fiber stacks carved from one malloc; fiber A yields to fiber B with an
//             inlined context switch (`mov sp, xN; br xM` inside the stepped line): a step over
//             the switch line must come back to A's next line once B switched back (F5d)

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

volatile int g_sw_trace = 0;
volatile int g_a_after = 0;

#define CASE_FN(i) __attribute__((noinline)) static int f##i(int k) { g_sw_trace = k * 2 + i; return g_sw_trace; }
CASE_FN(0) CASE_FN(1) CASE_FN(2) CASE_FN(3) CASE_FN(4) CASE_FN(5) CASE_FN(6) CASE_FN(7)
CASE_FN(8) CASE_FN(9) CASE_FN(10) CASE_FN(11) CASE_FN(12) CASE_FN(13) CASE_FN(14) CASE_FN(15)

__attribute__((noinline)) int dispatch(int k, int n)
{
  int r = 0;
  { volatile char buf[n]; buf[0] = (char)k; switch(k) { case 0: r = f0(k); break; case 1: r = f1(k); break; case 2: r = f2(k); break; case 3: r = f3(k); break; case 4: r = f4(k); break; case 5: r = f5(k); break; case 6: r = f6(k); break; case 7: r = f7(k); break; case 8: r = f8(k); break; case 9: r = f9(k); break; case 10: r = f10(k); break; case 11: r = f11(k); break; case 12: r = f12(k); break; case 13: r = f13(k); break; case 14: r = f14(k); break; case 15: r = f15(k); break; default: r = -1; break; } r += buf[0]; } // @vla_switch_line
  g_sw_trace = r;                       // @vla_after
  return r;
}

// an inlined context switch: saves the callee-saved registers and the continuation (the label
// after the block) on the current stack, stores sp into *from_sp, switches to to_sp, restores
// the registers saved there and jumps to the continuation saved there (`br`, like the Zig
// generator's inlined contextSwitch). The other context trashes the caller-saved registers.
#define CO_SWITCH(from_sp_ptr, to_sp) do { \
  uint64_t *co_from__ = (from_sp_ptr); uint64_t co_to__ = (to_sp); \
  __asm__ volatile( \
    "sub sp, sp, #0xa0\n" \
    "stp x19, x20, [sp, #0x00]\n" \
    "stp x21, x22, [sp, #0x10]\n" \
    "stp x23, x24, [sp, #0x20]\n" \
    "stp x25, x26, [sp, #0x30]\n" \
    "stp x27, x28, [sp, #0x40]\n" \
    "adr x9, 0f\n" \
    "stp x29, x9, [sp, #0x50]\n" \
    "stp d8,  d9,  [sp, #0x60]\n" \
    "stp d10, d11, [sp, #0x70]\n" \
    "stp d12, d13, [sp, #0x80]\n" \
    "stp d14, d15, [sp, #0x90]\n" \
    "mov x9, sp\n" \
    "str x9, [%0]\n" \
    "mov sp, %1\n" \
    "ldp x19, x20, [sp, #0x00]\n" \
    "ldp x21, x22, [sp, #0x10]\n" \
    "ldp x23, x24, [sp, #0x20]\n" \
    "ldp x25, x26, [sp, #0x30]\n" \
    "ldp x27, x28, [sp, #0x40]\n" \
    "ldp x29, x9,  [sp, #0x50]\n" \
    "ldp d8,  d9,  [sp, #0x60]\n" \
    "ldp d10, d11, [sp, #0x70]\n" \
    "ldp d12, d13, [sp, #0x80]\n" \
    "ldp d14, d15, [sp, #0x90]\n" \
    "add sp, sp, #0xa0\n" \
    "br x9\n" \
    "0:\n" \
    : : "r"(co_from__), "r"(co_to__) \
    : "memory", "cc", "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "x30", \
      "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23", "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31"); \
} while(0)

static uint64_t g_main_sp, g_a_sp, g_b_sp;

__attribute__((noinline, noreturn)) static void fiber_b_entry(void)
{
  for(;;)
  {
    g_sw_trace += 1;                    // @fiber_b_line
    CO_SWITCH(&g_b_sp, g_a_sp);         // @fiber_b_switch
  }
}

__attribute__((noinline, noreturn)) static void fiber_a_entry(void)
{
  for(int i = 0; i < 3; i += 1)
  {
    g_sw_trace = 10 + i;                // @fiber_a_line
    CO_SWITCH(&g_a_sp, g_b_sp);         // @fiber_a_switch
    g_a_after += 1;                     // @fiber_a_after
  }
  for(;;) { CO_SWITCH(&g_a_sp, g_main_sp); }
}

// the fiber's initial frame: the register save area CO_SWITCH pops, with the continuation = entry
static uint64_t fiber_init(char *stack_top, void (*entry)(void))
{
  uint64_t *frame = (uint64_t *)(stack_top - 0xa0);
  memset(frame, 0, 0xa0);
  frame[0x58/8] = (uint64_t)(uintptr_t)entry;
  return (uint64_t)(uintptr_t)frame;
}

int main(int argc, char **argv)
{
  const char *mode = argc > 1 ? argv[1] : "vla";
  if(strcmp(mode, "fibers") == 0)
  {
    char *stacks = malloc(2 * 64 * 1024);                 // two 64 KB fiber stacks from one allocation
    g_a_sp = fiber_init(stacks + 64 * 1024, fiber_a_entry);
    g_b_sp = fiber_init(stacks + 128 * 1024, fiber_b_entry);
    CO_SWITCH(&g_main_sp, g_a_sp);                        // @fiber_main_switch
    printf("fibers %d %d\n", g_sw_trace, g_a_after);      // @fiber_main_print
    return g_a_after;                                     // 3
  }
  int r = dispatch(argc + 6, 512 * 1024);                 // @vla_main_call (k = 7 when run with the mode only)
  printf("dispatch %d\n", r);
  return r & 0x7f;
}
