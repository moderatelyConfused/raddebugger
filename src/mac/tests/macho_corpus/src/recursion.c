// Deep recursion: direct, mutual and tail recursion; depth from argv[1]
// (default 20000, small frames).
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

static int64_t is_odd_depth(int32_t n, int64_t acc);

static int64_t
is_even_depth(int32_t n, int64_t acc)
{
  if(n == 0) { return acc; }
  return is_odd_depth(n - 1, acc + 2);
}

static int64_t
is_odd_depth(int32_t n, int64_t acc)
{
  if(n == 0) { return acc + 1; }
  return is_even_depth(n - 1, acc + 3);
}

__attribute__((noinline)) static int64_t
descend(int32_t depth, int64_t acc)
{
  volatile int64_t local = acc + depth; // keep a frame slot alive
  if(depth <= 0)
  {
    return local;
  }
  return descend(depth - 1, local ^ (acc << 1)) + 1;
}

static int64_t
tail_sum(int32_t n, int64_t acc)
{
  if(n == 0) { return acc; }
  return tail_sum(n - 1, acc + n);
}

int
main(int argc, char **argv)
{
  int32_t depth = (argc > 1) ? atoi(argv[1]) : 20000;
  int64_t a = descend(depth, argc);
  int64_t b = is_even_depth(depth, argc);
  int64_t c = tail_sum(depth, argc);
  printf("%s: depth=%d a=%lld b=%lld c=%lld\n", argv[0], depth, (long long)a, (long long)b, (long long)c);
  return (int)((a + b + c) & 0x7f);
}
