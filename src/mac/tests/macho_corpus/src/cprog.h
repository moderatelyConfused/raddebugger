// Shared header for the C corpus programs: inline functions defined in a
// header get inlined into several translation units, so the inlinee's file
// differs from the call site's (DW_AT_call_file != the inlined rows' file).
#ifndef CPROG_H
#define CPROG_H

#include <stdint.h>

typedef struct Vec2 Vec2;
struct Vec2
{
  int32_t x;
  int32_t y;
};

// a bitfield struct with an enum and array members: the checker's `types`
// column compares the RDI's member offsets (DW_AT_data_member_location,
// DW_AT_data_bit_offset), enumerator values and array counts with the DWARF
typedef enum Color
{
  Color_None  = -1,
  Color_Red   = 1,
  Color_Green = 2,
  Color_Blue  = 4,
  Color_All   = 7,
}
Color;

typedef struct Packed Packed;
struct Packed
{
  uint32_t kind  : 3;
  uint32_t flags : 5;
  uint32_t count : 24;
  uint8_t  tag;
  uint8_t  lo    : 2;
  uint8_t  hi    : 6;
  Color    color;
  int16_t  pair[2];
  Vec2     grid[2][3];
};

static inline int32_t
clamp_i32(int32_t v, int32_t lo, int32_t hi)
{
  if(v < lo) { v = lo; }
  if(v > hi) { v = hi; }
  return v;
}

static inline Vec2
vec2_add(Vec2 a, Vec2 b)
{
  Vec2 r;
  r.x = clamp_i32(a.x + b.x, -1000, 1000);
  r.y = clamp_i32(a.y + b.y, -1000, 1000);
  return r;
}

static inline uint32_t
hash_mix(uint32_t h, uint32_t v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  return h;
}

// defined in cprog_util.c
int32_t util_sum_squares(int32_t n);
uint32_t util_hash_buffer(const uint8_t *data, uint64_t size);
Vec2 util_walk(Vec2 start, int32_t steps);

#endif // CPROG_H
