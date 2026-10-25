// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#ifndef ARM64_DISASM_H
#define ARM64_DISASM_H

////////////////////////////////
//~ rjf: Instruction Decoding
//
// Decodes exactly one A64 instruction (4 bytes, little-endian) at `code.str`,
// producing ARM-syntax text and the control-flow / stack-pointer analysis used
// by the debug engine for stepping. `syntax` is ignored (A64 has a single
// assembler syntax). The decoder never fails: unknown encodings produce a
// `.inst 0x........` instruction of size 4 with no flags.

internal DASM_Inst arm64_dasm_inst_from_code(Arena *arena, U64 vaddr, String8 code, DASM_Syntax syntax);

#endif // ARM64_DISASM_H
