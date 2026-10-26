// flows debuggee (F5b): a function whose first line allocates a 512 KB local array and jumps
// through a table (a computed goto: clang lowers small switches on arm64 to compare trees, so the
// indirect jump is written out). Exits with 10 + argc (11 without arguments).

#include <stdio.h>

volatile int g_big_trace = 0;

__attribute__((noinline)) int big_frame(int sel) { volatile char buf[512 * 1024]; int r = 0; static void *const table[4] = { &&l0, &&l1, &&l2, &&l3 }; buf[(512 * 1024) - 1] = 1; goto *table[sel & 3]; l0: r = 10; goto done; l1: r = 11; goto done; l2: r = 12; goto done; l3: r = 13; done: // @big_first_line
  buf[0] = (char)r;                   // @big_second_line
  g_big_trace = buf[0] + buf[(512 * 1024) - 1] - 1; // @big_third_line
  return g_big_trace;
}

int main(int argc, char **argv)
{
  (void)argv;
  int r = big_frame(argc);            // @big_main_call
  printf("big %d\n", r);              // @big_main_print
  return r;
}
