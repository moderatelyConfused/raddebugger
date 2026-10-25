// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

////////////////////////////////
//~ rjf: Generated

#include "arm64/generated/arm64.meta.c"

////////////////////////////////
//~ rjf: Helpers

internal B32
arm64_inst_is_brk(U32 inst)
{
  // BRK #imm16: 1101 0100 001 imm16 000 00
  return ((inst & 0xffe0001fu) == 0xd4200000u);
}

internal U32
arm64_brk_imm_from_inst(U32 inst)
{
  return (inst >> 5) & 0xffff;
}
