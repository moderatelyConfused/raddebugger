// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#ifndef FONT_PROVIDER_CORETEXT_H
#define FONT_PROVIDER_CORETEXT_H

////////////////////////////////
//~ rjf: Includes
//
// CoreText / CoreGraphics are framework headers and cannot be included from
// the unity build; the rasterization lives in src/mac/mac_platform.m behind
// the plain C API below.

#include "mac/mac_platform.h"

////////////////////////////////
//~ rjf: State Types

typedef struct FP_CT_State FP_CT_State;
struct FP_CT_State
{
  Arena *arena;
};

////////////////////////////////
//~ rjf: Globals

global FP_CT_State *fp_ct_state = 0;

////////////////////////////////
//~ rjf: Helpers

internal MAC_PLAT_Font *fp_ct_font_from_handle(FP_Handle handle);
internal FP_Handle fp_ct_handle_from_font(MAC_PLAT_Font *font);
internal void *fp_ct_arena_alloc(void *user, size_t size);

#endif // FONT_PROVIDER_CORETEXT_H
