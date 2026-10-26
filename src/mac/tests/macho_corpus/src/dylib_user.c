// Executable linked against libshape.dylib.
#include <stdio.h>
#include "libshape.h"

static int64_t
total_area(int32_t n)
{
  int64_t t = 0;
  for(int32_t i = 1; i <= n; i += 1)
  {
    t += shape_rect_area(i, -i) + shape_circle_area(i);
  }
  return t;
}

int
main(int argc, char **argv)
{
  int64_t t = total_area(argc + 2);
  printf("%s: total=%lld calls=%d\n", argv[0], (long long)t, shape_call_count());
  return (int)(t & 0x7f);
}
