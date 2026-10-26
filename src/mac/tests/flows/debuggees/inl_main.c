#include "inl.h"
volatile int g_inl = 0; int main(int argc, char **argv) { (void)argv; int r = 0;
  r += inl_add(&g_inl, argc);
  r += inl_add(&g_inl, 2);
  return r & 0x7f; }
// flows debuggee (F4b): inl.h:3 is inlined at inl_main.c:3 (the inlinee's line number equals the call site's).
