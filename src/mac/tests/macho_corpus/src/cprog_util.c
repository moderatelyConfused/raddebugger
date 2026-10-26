// Second translation unit of the C corpus program.
#include "cprog.h"

static int32_t
square(int32_t v)
{
  return v*v;
}

int32_t
util_sum_squares(int32_t n)
{
  int32_t sum = 0;
  for(int32_t i = 0; i < n; i += 1)
  {
    sum += square(i);
  }
  return sum;
}

uint32_t
util_hash_buffer(const uint8_t *data, uint64_t size)
{
  uint32_t h = 0;
  for(uint64_t i = 0; i < size; i += 1)
  {
    h = hash_mix(h, data[i]);
  }
  return h;
}

Vec2
util_walk(Vec2 start, int32_t steps)
{
  Vec2 p = start;
  Vec2 d = {1, 2};
  for(int32_t i = 0; i < steps; i += 1)
  {
    p = vec2_add(p, d);
    if((i & 3) == 3)
    {
      d.x = -d.x;
    }
  }
  return p;
}
