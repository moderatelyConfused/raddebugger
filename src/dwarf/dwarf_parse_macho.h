// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#ifndef DWARF_PARSE_MACHO_H
#define DWARF_PARSE_MACHO_H

internal String8 dw_regular_section_name_from_macho_section_name(String8 macho_name); // "__debug_str_offs" -> ".debug_str_offsets", ...; empty if not a DWARF section
internal B32 dw_is_dwarf_present_from_macho_bin(String8 raw_image, MACHO_Bin *bin);
internal DW_Raw dw_raw_from_macho_bin(Arena *arena, String8 raw_image, MACHO_Bin *bin);

#endif // DWARF_PARSE_MACHO_H
