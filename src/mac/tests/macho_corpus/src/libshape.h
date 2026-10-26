#ifndef LIBSHAPE_H
#define LIBSHAPE_H
#include <stdint.h>
extern int32_t shape_g_calls;
int64_t shape_rect_area(int32_t w, int32_t h);
int64_t shape_circle_area(int32_t r);
int32_t shape_call_count(void);
#endif
