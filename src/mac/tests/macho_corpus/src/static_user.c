// Executable linked against the static library libstat.a.
#include <stdio.h>
#include "libstat.h"

int
main(int argc, char **argv)
{
  uint32_t v = (uint32_t)argc * 0x12345u;
  int32_t bits = 0;
  for(int i = 0; i < 8; i += 1)
  {
    v = stat_scramble(v);
    bits += stat_count_bits(v);
  }
  printf("%s: v=%u bits=%d\n", argv[0], v, bits);
  return bits & 0x7f;
}
