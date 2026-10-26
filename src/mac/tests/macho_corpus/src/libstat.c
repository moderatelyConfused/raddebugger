// Static library member (libstat.a): the executable's debug map then has
// N_OSO entries of the form `path/libstat.a(libstat.o)`.
#include <stdint.h>
#include "libstat.h"

int32_t stat_g_seed = 17;

static uint32_t
rotl(uint32_t v, int s)
{
  return (v << s) | (v >> (32 - s));
}

uint32_t
stat_scramble(uint32_t v)
{
  v ^= (uint32_t)stat_g_seed;
  v = rotl(v, 5) * 0x27d4eb2du;
  v = rotl(v, 13) ^ (v >> 7);
  return v;
}

int32_t
stat_count_bits(uint32_t v)
{
  int32_t n = 0;
  for(; v != 0; v &= v - 1)
  {
    n += 1;
  }
  return n;
}
