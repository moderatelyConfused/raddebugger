// C corpus program: several procedures, inline functions from a header (so
// inlined code comes from another file), a static function inlined into more
// than one caller, nested inlining, a switch, loops, function pointers,
// globals (data, bss, const) and a thread-local variable. Everything depends
// on argc so optimizers cannot fold the program away.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cprog.h"

int32_t g_counter = 3;              // __DATA,__data
int32_t g_table[64];                // __DATA,__bss (or __common)
const char *const g_name = "cprog"; // const data
static int32_t s_hidden = 11;       // static, internal linkage
_Thread_local int32_t t_slot = 7;   // __DATA,__thread_vars / __thread_data
_Thread_local int32_t t_zero;       // __thread_bss

typedef int32_t (*BinOp)(int32_t, int32_t);

static int32_t
op_add(int32_t a, int32_t b)
{
  return a + b;
}

static int32_t
op_mul(int32_t a, int32_t b)
{
  return a * b;
}

static int32_t
twice(int32_t v)
{
  // inlined into more than one caller at -O2
  return clamp_i32(v*2, -500, 500);
}

__attribute__((noinline)) int32_t
dispatch(int32_t kind, int32_t a, int32_t b)
{
  BinOp op = 0;
  switch(kind)
  {
    case 0: op = op_add; break;
    case 1: op = op_mul; break;
    case 2: return twice(a) + twice(b);
    default: return clamp_i32(a - b, -10, 10);
  }
  return op(a, b);
}

__attribute__((noinline)) int32_t
fill_table(int32_t seed)
{
  int32_t acc = seed;
  for(int32_t i = 0; i < 64; i += 1)
  {
    g_table[i] = twice(acc + i);
    acc = hash_mix((uint32_t)acc, (uint32_t)g_table[i]) & 0xffff;
  }
  return acc;
}

static int32_t
thread_local_bump(int32_t by)
{
  t_slot += by;
  t_zero += 1;
  return t_slot + t_zero;
}

int
main(int argc, char **argv)
{
  int32_t seed = argc + g_counter + s_hidden;
  int32_t sum = util_sum_squares(seed);
  uint32_t h = util_hash_buffer((const uint8_t *)argv[0], strlen(argv[0]));
  Vec2 start = {seed, -seed};
  Vec2 end = util_walk(start, seed*3);
  int32_t d0 = dispatch(0, seed, sum);
  int32_t d1 = dispatch(1, seed, (int32_t)h);
  int32_t d2 = dispatch(2, seed, end.x);
  int32_t d3 = dispatch(argc + 5, end.x, end.y);
  int32_t acc = fill_table(seed);
  int32_t tl = thread_local_bump(argc);
  printf("%s: seed=%d sum=%d h=%u end=(%d,%d) d=%d,%d,%d,%d acc=%d tl=%d\n",
         g_name, seed, sum, h, end.x, end.y, d0, d1, d2, d3, acc, tl);
  return (d0 + d1 + d2 + d3 + acc + tl) & 0x7f;
}
