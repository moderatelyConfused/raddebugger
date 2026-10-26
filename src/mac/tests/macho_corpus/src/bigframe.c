// Large frames, variable-length arrays and alloca: frame sizes beyond the
// immediate range of `sub sp`, and dynamically sized frames.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <alloca.h>

__attribute__((noinline)) static uint32_t
checksum(const uint8_t *p, size_t n)
{
  uint32_t h = 2166136261u;
  for(size_t i = 0; i < n; i += 1)
  {
    h = (h ^ p[i]) * 16777619u;
  }
  return h;
}

__attribute__((noinline)) static uint32_t
large_frame(int seed)
{
  uint8_t big[1 << 17];      // 128 KiB, well past the 4 KiB `sub sp, sp, #imm` range
  uint8_t small[64];
  memset(big, (uint8_t)seed, sizeof(big));
  memset(small, (uint8_t)(seed + 1), sizeof(small));
  big[seed & 0xff] = small[seed & 63];
  return checksum(big, sizeof(big)) ^ checksum(small, sizeof(small));
}

__attribute__((noinline)) static uint32_t
vla_frame(int n)
{
  int32_t v[n];                // VLA
  uint8_t bytes[n * 3];        // second VLA
  for(int i = 0; i < n; i += 1)
  {
    v[i] = i * n;
    bytes[i*3 + 0] = (uint8_t)i;
    bytes[i*3 + 1] = (uint8_t)(i >> 1);
    bytes[i*3 + 2] = (uint8_t)(i >> 2);
  }
  return checksum((const uint8_t *)v, sizeof(v)) + checksum(bytes, sizeof(bytes));
}

__attribute__((noinline)) static uint32_t
alloca_frame(int n)
{
  uint32_t acc = 0;
  for(int round = 0; round < 3; round += 1)
  {
    uint8_t *p = (uint8_t *)alloca((size_t)n * (round + 1));
    memset(p, round + n, (size_t)n * (round + 1));
    acc += checksum(p, (size_t)n * (round + 1));
  }
  return acc;
}

int
main(int argc, char **argv)
{
  int n = 16 + argc * 8;
  uint32_t a = large_frame(argc);
  uint32_t b = vla_frame(n);
  uint32_t c = alloca_frame(n);
  printf("%s: %u %u %u\n", argv[0], a, b, c);
  return (int)((a ^ b ^ c) & 0x7f);
}
