// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

// NOTE: Mach-O section names are capped at 16 characters, so some DWARF
// section names are truncated relative to their ELF spelling (most notably
// `__debug_str_offs` for `.debug_str_offsets`). The `__apple_*` accelerator
// tables are intentionally not mapped.
typedef struct DW_MachoSectionNameMapEntry DW_MachoSectionNameMapEntry;
struct DW_MachoSectionNameMapEntry
{
  String8 macho_name;
  String8 regular_name;
};

read_only global DW_MachoSectionNameMapEntry dw_macho_section_name_map[] =
{
    {str8_lit_comp("__debug_abbrev"),   str8_lit_comp(".debug_abbrev")},
    {str8_lit_comp("__debug_aranges"),  str8_lit_comp(".debug_aranges")},
    {str8_lit_comp("__debug_frame"),    str8_lit_comp(".debug_frame")},
    {str8_lit_comp("__debug_info"),     str8_lit_comp(".debug_info")},
    {str8_lit_comp("__debug_line"),     str8_lit_comp(".debug_line")},
    {str8_lit_comp("__debug_loc"),      str8_lit_comp(".debug_loc")},
    {str8_lit_comp("__debug_macinfo"),  str8_lit_comp(".debug_macinfo")},
    {str8_lit_comp("__debug_macro"),    str8_lit_comp(".debug_macro")},
    {str8_lit_comp("__debug_pubnames"), str8_lit_comp(".debug_pubnames")},
    {str8_lit_comp("__debug_pubtypes"), str8_lit_comp(".debug_pubtypes")},
    {str8_lit_comp("__debug_ranges"),   str8_lit_comp(".debug_ranges")},
    {str8_lit_comp("__debug_str"),      str8_lit_comp(".debug_str")},
    {str8_lit_comp("__debug_types"),    str8_lit_comp(".debug_types")},
    {str8_lit_comp("__debug_addr"),     str8_lit_comp(".debug_addr")},
    {str8_lit_comp("__debug_loclists"), str8_lit_comp(".debug_loclists")},
    {str8_lit_comp("__debug_rnglists"), str8_lit_comp(".debug_rnglists")},
    {str8_lit_comp("__debug_str_offs"), str8_lit_comp(".debug_str_offsets")},
    {str8_lit_comp("__debug_line_str"), str8_lit_comp(".debug_line_str")},
    {str8_lit_comp("__debug_names"),    str8_lit_comp(".debug_names")},
    {str8_lit_comp("__debug_cu_index"), str8_lit_comp(".debug_cu_index")},
    {str8_lit_comp("__debug_tu_index"), str8_lit_comp(".debug_tu_index")},
};

internal String8
dw_regular_section_name_from_macho_section_name(String8 macho_name)
{
  String8 result = {0};
  for EachIndex(idx, ArrayCount(dw_macho_section_name_map))
  {
    if(str8_match(macho_name, dw_macho_section_name_map[idx].macho_name, 0))
    {
      result = dw_macho_section_name_map[idx].regular_name;
      break;
    }
  }
  return result;
}

internal B32
dw_is_dwarf_present_from_macho_bin(String8 data, MACHO_Bin *bin)
{
  B32 is_dwarf_present = 0;
  for EachIndex(idx, bin->sections.count)
  {
    MACHO_Section64 *section = &bin->sections.v[idx];
    if(!str8_match(macho_string_from_name16(section->segname), str8_lit("__DWARF"), 0)) { continue; }
    String8 name = dw_regular_section_name_from_macho_section_name(macho_string_from_name16(section->sectname));
    DW_SectionKind s = dw_section_kind_from_string(name);
    is_dwarf_present = (s != DW_SectionKind_Null && section->size != 0);
    if(is_dwarf_present)
    {
      break;
    }
  }
  return is_dwarf_present;
}

internal DW_Raw
dw_raw_from_macho_bin(Arena *arena, String8 data, MACHO_Bin *bin)
{
  // NOTE: unlike ELF, Mach-O DWARF sections are never compressed, so the
  // section file data is used as-is. The first occurrence of a section wins.
  DW_Raw result = {0};
  B32 is_section_present[ArrayCount(result.sec)] = {0};
  for EachIndex(idx, bin->sections.count)
  {
    MACHO_Section64 *section = &bin->sections.v[idx];
    if(!str8_match(macho_string_from_name16(section->segname), str8_lit("__DWARF"), 0)) { continue; }

    //- rjf: unpack section
    String8 section_name = dw_regular_section_name_from_macho_section_name(macho_string_from_name16(section->sectname));
    DW_SectionKind section_kind = dw_section_kind_from_string(section_name);
    if(section_kind == DW_SectionKind_Null) { continue; } // skip unknown / __apple_* sections
    if(is_section_present[section_kind]) { continue; }    // skip duplicate sections
    String8 section_data = macho_data_from_section(data, bin, section);

    //- rjf: store
    is_section_present[section_kind] = 1;
    DW_Section *d = &result.sec[section_kind];
    d->name = push_str8_copy(arena, section_name);
    d->data = section_data;
  }
  return result;
}
