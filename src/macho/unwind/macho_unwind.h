// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#ifndef MACHO_UNWIND_H
#define MACHO_UNWIND_H

////////////////////////////////
//~ rjf: Apple Compact Unwind Format (__TEXT,__unwind_info)
//
// See <mach-o/compact_unwind_encoding.h>. Function offsets in this section
// are relative to the image's mach header (the start of __TEXT).

#define MACHO_UWND_SECTION_VERSION          1
#define MACHO_UWND_SECOND_LEVEL_REGULAR     2
#define MACHO_UWND_SECOND_LEVEL_COMPRESSED  3

typedef struct MACHO_UWND_SectionHeader MACHO_UWND_SectionHeader;
struct MACHO_UWND_SectionHeader
{
  U32 version;
  U32 common_encodings_array_section_offset;
  U32 common_encodings_array_count;
  U32 personality_array_section_offset;
  U32 personality_array_count;
  U32 index_section_offset;
  U32 index_count;
};

typedef struct MACHO_UWND_IndexEntry MACHO_UWND_IndexEntry;
struct MACHO_UWND_IndexEntry
{
  U32 function_offset;
  U32 second_level_pages_section_offset; // 0 in the final (sentinel) entry
  U32 lsda_index_array_section_offset;
};

typedef struct MACHO_UWND_RegularPageHeader MACHO_UWND_RegularPageHeader;
struct MACHO_UWND_RegularPageHeader
{
  U32 kind;
  U16 entry_page_offset;
  U16 entry_count;
};

typedef struct MACHO_UWND_RegularEntry MACHO_UWND_RegularEntry;
struct MACHO_UWND_RegularEntry
{
  U32 function_offset;
  U32 encoding;
};

typedef struct MACHO_UWND_CompressedPageHeader MACHO_UWND_CompressedPageHeader;
struct MACHO_UWND_CompressedPageHeader
{
  U32 kind;
  U16 entry_page_offset;
  U16 entry_count;
  U16 encodings_page_offset;
  U16 encodings_count;
};

#define MACHO_UWND_COMPRESSED_ENTRY_FUNC_OFFSET(e)    ((e) & 0x00ffffffu)
#define MACHO_UWND_COMPRESSED_ENTRY_ENCODING_INDEX(e) (((e) >> 24) & 0xffu)

//- rjf: generic encoding bits
#define MACHO_UWND_IS_NOT_FUNCTION_START 0x80000000u
#define MACHO_UWND_HAS_LSDA              0x40000000u
#define MACHO_UWND_PERSONALITY_MASK      0x30000000u

//- rjf: arm64 encodings
#define MACHO_UWND_ARM64_MODE_MASK                 0x0f000000u
#define MACHO_UWND_ARM64_MODE_FRAMELESS            0x02000000u
#define MACHO_UWND_ARM64_MODE_DWARF                0x03000000u
#define MACHO_UWND_ARM64_MODE_FRAME                0x04000000u
#define MACHO_UWND_ARM64_FRAME_X19_X20_PAIR        0x00000001u
#define MACHO_UWND_ARM64_FRAME_X21_X22_PAIR        0x00000002u
#define MACHO_UWND_ARM64_FRAME_X23_X24_PAIR        0x00000004u
#define MACHO_UWND_ARM64_FRAME_X25_X26_PAIR        0x00000008u
#define MACHO_UWND_ARM64_FRAME_X27_X28_PAIR        0x00000010u
#define MACHO_UWND_ARM64_FRAME_D8_D9_PAIR          0x00000100u
#define MACHO_UWND_ARM64_FRAME_D10_D11_PAIR        0x00000200u
#define MACHO_UWND_ARM64_FRAME_D12_D13_PAIR        0x00000400u
#define MACHO_UWND_ARM64_FRAME_D14_D15_PAIR        0x00000800u
#define MACHO_UWND_ARM64_FRAMELESS_STACK_SIZE_MASK 0x00fff000u
#define MACHO_UWND_ARM64_DWARF_SECTION_OFFSET      0x00ffffffu

////////////////////////////////
//~ rjf: Module Unwind Info Type

typedef struct MACHO_UWND_ModuleUnwindInfo MACHO_UWND_ModuleUnwindInfo;
struct MACHO_UWND_ModuleUnwindInfo
{
  String8 unwind_info_data;      // copy of __TEXT,__unwind_info (may be empty)
  Rng1U64 eh_frame_vaddr_range;  // __TEXT,__eh_frame in the process (may be empty)
  EH_PtrCtx ptr_ctx;
};

////////////////////////////////
//~ rjf: Lookups

internal B32 macho_uwnd_encoding_from_voff(MACHO_UWND_ModuleUnwindInfo *info, U64 voff, U32 *encoding_out);

////////////////////////////////
//~ rjf: Unwinding Abstraction Implementation

internal UWND_StepResult macho_uwnd_step(Arch arch, MemoryMap *memory_map, UWND_ModuleInfo *module_info, U64 tls_vaddr, void *regs, U64 *cfa_out);

#endif // MACHO_UNWIND_H
