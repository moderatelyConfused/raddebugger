// Dynamic library of the corpus (built as libshape.dylib): exported
// procedures, an exported global, a thread-local, and an internal helper
// which is inlined into the exported procedures at -O2.
#include <stdint.h>
#include "libshape.h"

int32_t shape_g_calls = 0;
_Thread_local int32_t shape_t_last = -1;

static int32_t
shape_abs(int32_t v)
{
  return v < 0 ? -v : v;
}

int64_t
shape_rect_area(int32_t w, int32_t h)
{
  shape_g_calls += 1;
  shape_t_last = w;
  return (int64_t)shape_abs(w) * shape_abs(h);
}

int64_t
shape_circle_area(int32_t r)
{
  shape_g_calls += 1;
  shape_t_last = r;
  int64_t rr = (int64_t)shape_abs(r);
  return 3*rr*rr;
}

int32_t
shape_call_count(void)
{
  return shape_g_calls + shape_t_last;
}
