// flows debuggee: a coroutine on a stack array local to main, switched with `mov sp, xN; br xM` (F5a).
// Exits with 6.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

volatile int g_co_trace = 0;
static uint64_t g_main_sp, g_co_sp;

// saves the callee-saved registers on the current stack, stores sp into *from_sp, switches to
// to_sp, restores the registers saved there and jumps to the saved return address (an indirect
// jump, like a fiber library's context switch)
__attribute__((naked, noinline)) void co_switch(uint64_t *from_sp, uint64_t to_sp)
{
  __asm__ volatile(
    "sub sp, sp, #0xa0\n"
    "stp x19, x20, [sp, #0x00]\n"
    "stp x21, x22, [sp, #0x10]\n"
    "stp x23, x24, [sp, #0x20]\n"
    "stp x25, x26, [sp, #0x30]\n"
    "stp x27, x28, [sp, #0x40]\n"
    "stp x29, x30, [sp, #0x50]\n"
    "stp d8,  d9,  [sp, #0x60]\n"
    "stp d10, d11, [sp, #0x70]\n"
    "stp d12, d13, [sp, #0x80]\n"
    "stp d14, d15, [sp, #0x90]\n"
    "mov x9, sp\n"
    "str x9, [x0]\n"
    "mov sp, x1\n"
    "ldp x19, x20, [sp, #0x00]\n"
    "ldp x21, x22, [sp, #0x10]\n"
    "ldp x23, x24, [sp, #0x20]\n"
    "ldp x25, x26, [sp, #0x30]\n"
    "ldp x27, x28, [sp, #0x40]\n"
    "ldp x29, x30, [sp, #0x50]\n"
    "ldp d8,  d9,  [sp, #0x60]\n"
    "ldp d10, d11, [sp, #0x70]\n"
    "ldp d12, d13, [sp, #0x80]\n"
    "ldp d14, d15, [sp, #0x90]\n"
    "add sp, sp, #0xa0\n"
    "br x30\n");
}

static void co_body(void)
{
  for(int i = 1; i <= 3; i += 1)
  {
    g_co_trace = i;                   // @co_body_line
    co_switch(&g_co_sp, g_main_sp);   // @co_yield
  }
}

__attribute__((noreturn, noinline)) static void co_entry(void)
{
  co_body();
  for(;;) { co_switch(&g_co_sp, g_main_sp); }
}

int main(int argc, char **argv)
{
  (void)argc; (void)argv;
  uint64_t stack[8192] __attribute__((aligned(16)));   // 64 KB inside main's frame
  memset(stack, 0, sizeof(stack));
  // the coroutine's initial frame: the register save area co_switch pops, with x30 = co_entry
  uint64_t *frame = stack + (sizeof(stack)/sizeof(stack[0])) - 0xa0/8;
  frame[0x58/8] = (uint64_t)(uintptr_t)&co_entry;
  g_co_sp = (uint64_t)(uintptr_t)frame;
  int sum = 0;
  for(int round = 0; round < 3; round += 1)
  {
    co_switch(&g_main_sp, g_co_sp);  // @main_switch
    sum += g_co_trace;               // @main_after_switch
  }
  printf("coro %d\n", sum);          // @main_print
  return sum;                        // @main_ret
}
