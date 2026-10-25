// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#ifndef MACHO_PARSE_H
#define MACHO_PARSE_H

////////////////////////////////
//~ rjf: Parsed Structure Types

typedef struct MACHO_FatArchArray MACHO_FatArchArray;
struct MACHO_FatArchArray
{
  U64 count;
  MACHO_FatArch64 *v; // decoded to host byte order; 32-bit fat_arch entries are widened
};

typedef struct MACHO_LoadCommandInfo MACHO_LoadCommandInfo;
struct MACHO_LoadCommandInfo
{
  U32 cmd;
  U32 cmdsize;
  U64 off; // offset of the command, relative to the start of the slice
};

typedef struct MACHO_LoadCommandInfoArray MACHO_LoadCommandInfoArray;
struct MACHO_LoadCommandInfoArray
{
  U64 count;
  MACHO_LoadCommandInfo *v;
};

typedef struct MACHO_Segment MACHO_Segment;
struct MACHO_Segment
{
  MACHO_SegmentCommand64 cmd;
  U64 first_section_idx; // index into MACHO_Bin.sections of this segment's first section
  U64 section_count;     // number of sections owned by this segment (may be < cmd.nsects if truncated)
};

typedef struct MACHO_SegmentArray MACHO_SegmentArray;
struct MACHO_SegmentArray
{
  U64 count;
  MACHO_Segment *v;
};

typedef struct MACHO_Section64Array MACHO_Section64Array;
struct MACHO_Section64Array
{
  U64 count;
  MACHO_Section64 *v; // flat, in load command order; nlist n_sect == index + 1
};

typedef struct MACHO_NList64Array MACHO_NList64Array;
struct MACHO_NList64Array
{
  U64 count;
  MACHO_NList64 *v;
};

typedef struct MACHO_Bin MACHO_Bin;
struct MACHO_Bin
{
  //- rjf: slice selection (fat binaries). all file offsets stored in this
  // structure (and in the load commands) are relative to `slice`, which is
  // `str8_substr(data, slice_range)` of the data that was parsed. for thin
  // files, slice_range == [0, data.size).
  B32 is_valid; // 64-bit Mach-O header found & parsed
  B32 is_fat;
  U64 fat_arch_count;
  MACHO_FatArch64 fat_arch; // the selected slice's fat_arch entry, if is_fat
  Rng1U64 slice_range;
  String8 slice;

  //- rjf: header
  MACHO_Header64 hdr;
  B32 is_pie;

  //- rjf: load commands
  MACHO_LoadCommandInfoArray load_commands;
  MACHO_SegmentArray segments;
  MACHO_Section64Array sections;

  //- rjf: image base; vmaddr of the `__TEXT` segment (first segment if none;
  // 0 for MH_OBJECT files). "voff" == vaddr - base_vaddr.
  B32 has_text_segment;
  U64 text_segment_idx;
  U64 base_vaddr;

  //- rjf: symbol table
  B32 has_symtab;
  MACHO_SymtabCommand symtab;
  MACHO_NList64Array symbols;
  String8 string_table;
  B32 has_dysymtab;
  MACHO_DysymtabCommand dysymtab;
  U64 stab_count;      // number of symbolic debugging (stab) entries
  U64 oso_count;       // number of N_OSO stabs (debug map object file references)
  B32 has_debug_map;   // oso_count != 0

  //- rjf: identity
  B32 has_uuid;
  Guid uuid;

  //- rjf: entry point (LC_MAIN or LC_UNIXTHREAD); voff relative to base_vaddr
  B32 has_entry_point;
  U64 entry_point_voff;
  U64 entry_point_stacksize;

  //- rjf: dyld info
  String8 dylib_install_name; // LC_ID_DYLIB (MH_DYLIB only)
  String8 dylinker_name;      // LC_LOAD_DYLINKER
  String8List dylibs;         // LC_LOAD_DYLIB / LC_LOAD_WEAK_DYLIB / LC_REEXPORT_DYLIB / LC_LOAD_UPWARD_DYLIB / LC_LAZY_LOAD_DYLIB
  String8List rpaths;         // LC_RPATH
  B32 has_dyld_info;
  MACHO_DyldInfoCommand dyld_info;

  //- rjf: versions
  B32 has_build_version;
  MACHO_BuildVersionCommand build_version;
  B32 has_version_min;
  MACHO_VersionMinCommand version_min;
  B32 has_source_version;
  U64 source_version;

  //- rjf: __LINKEDIT data ranges (relative to slice); zero if absent
  B32 has_function_starts;
  Rng1U64 function_starts_range;
  Rng1U64 data_in_code_range;
  Rng1U64 code_signature_range;
  Rng1U64 exports_trie_range;
  Rng1U64 chained_fixups_range;
  Rng1U64 segment_split_info_range;

  //- rjf: decoded LC_FUNCTION_STARTS; voffs relative to base_vaddr, ascending
  U64Array function_starts;
};

////////////////////////////////
//~ rjf: Fat (Universal) Binary Parsing

internal MACHO_FatArchArray macho_fat_arch_array_from_data(Arena *arena, String8 data);
internal U64 macho_fat_arch_idx_from_array(MACHO_FatArchArray *arches); // picks arm64 (non-arm64e) > arm64 (any) > x86_64; returns arches->count if none

////////////////////////////////
//~ rjf: Top-Level Binary Parsing

internal MACHO_Bin macho_bin_from_data(Arena *arena, String8 data);

////////////////////////////////
//~ rjf: Segment / Section Helpers

internal String8 macho_slice_from_data(String8 data, MACHO_Bin *bin);
internal MACHO_Segment *macho_segment_from_name(MACHO_Bin *bin, String8 segname);
internal MACHO_Segment *macho_segment_from_section_idx(MACHO_Bin *bin, U64 section_idx);
internal MACHO_Section64 *macho_section_from_name(MACHO_Bin *bin, String8 segname, String8 sectname);
internal U64 macho_section_idx_from_section(MACHO_Bin *bin, MACHO_Section64 *section); // returns bin->sections.count if not from this bin
internal String8 macho_name_from_section(Arena *arena, MACHO_Section64 *section); // "segname,sectname"
internal String8 macho_data_from_segment(String8 data, MACHO_Bin *bin, MACHO_Segment *segment);
internal String8 macho_data_from_section(String8 data, MACHO_Bin *bin, MACHO_Section64 *section); // empty for zero-fill sections / sections w/o file data (e.g. dSYM copies)

////////////////////////////////
//~ rjf: Symbol Helpers

internal String8 macho_name_from_nlist(MACHO_Bin *bin, MACHO_NList64 *nlist);
internal B32 macho_is_dwarf_present_from_bin(MACHO_Bin *bin); // `__DWARF,__debug_info` section exists
internal B32 macho_has_debug_map_from_bin(MACHO_Bin *bin);    // any N_OSO stab (linked w/o dsymutil)

////////////////////////////////
//~ rjf: dSYM Paths

internal String8 macho_dsym_path_from_exe_path(Arena *arena, String8 exe_path); // "<exe>.dSYM/Contents/Resources/DWARF/<basename of exe>"

#endif // MACHO_PARSE_H
