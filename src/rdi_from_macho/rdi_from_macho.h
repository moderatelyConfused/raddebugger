// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#ifndef RDI_FROM_MACHO_H
#define RDI_FROM_MACHO_H

////////////////////////////////
//~ rjf: Symbol Table Conversion Types

typedef struct M2R_SymtabConvertParams M2R_SymtabConvertParams;
struct M2R_SymtabConvertParams
{
  String8 exe_name;
  String8 data; // the Mach-O image (thin or fat; `bin` was parsed from it)
  MACHO_Bin *bin;
  RDIM_SubsetFlags subset_flags;
  B32 deterministic;
};

typedef struct M2R_SymtabProc M2R_SymtabProc;
struct M2R_SymtabProc
{
  U64 vaddr;
  U64 section_idx;
  U64 nlist_idx;
};

////////////////////////////////
//~ rjf: Binary Sections

internal RDIM_BinarySectionList m2r_rdi_binary_sections_from_macho(Arena *arena, String8 data, MACHO_Bin *bin);

////////////////////////////////
//~ rjf: Symbol Table -> RDI (images without DWARF)

internal int m2r_symtab_proc_compare(M2R_SymtabProc *a, M2R_SymtabProc *b);
internal RDIM_BakeParams m2r_bake_params_from_macho_symtab(Arena *arena, M2R_SymtabConvertParams *params);

#endif // RDI_FROM_MACHO_H
