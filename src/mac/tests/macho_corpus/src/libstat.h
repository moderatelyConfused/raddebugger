#ifndef LIBSTAT_H
#define LIBSTAT_H
#include <stdint.h>
extern int32_t stat_g_seed;
uint32_t stat_scramble(uint32_t v);
int32_t stat_count_bits(uint32_t v);
#endif
