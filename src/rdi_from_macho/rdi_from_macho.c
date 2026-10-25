// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

internal RDIM_BinarySectionList
m2r_rdi_binary_sections_from_macho(Arena *arena, String8 data, MACHO_Bin *bin)
{
  // NOTE: one RDI binary section per Mach-O section (segments are not
  // emitted themselves; e.g. __PAGEZERO has no sections). voffs are relative
  // to the image base (the __TEXT segment's vmaddr). sections of the __DWARF
  // segment are debug data rather than image contents, so they are skipped.
  RDIM_BinarySectionList result = {0};
  for EachIndex(seg_idx, bin->segments.count)
  {
    MACHO_Segment *segment = &bin->segments.v[seg_idx];

    // rjf: map segment protection -> rdi flags
    RDI_BinarySectionFlags flags = RDI_BinarySectionFlag_Read;
    if(segment->cmd.initprot & MACHO_VM_PROT_WRITE)   { flags |= RDI_BinarySectionFlag_Write; }
    if(segment->cmd.initprot & MACHO_VM_PROT_EXECUTE) { flags |= RDI_BinarySectionFlag_Execute; }

    for EachIndex(idx, segment->section_count)
    {
      // rjf: unpack section
      MACHO_Section64 *src_section = &bin->sections.v[segment->first_section_idx + idx];
      String8 segname  = macho_string_from_name16(src_section->segname);
      String8 sectname = macho_string_from_name16(src_section->sectname);
      if(str8_match(segname, str8_lit("__DWARF"), 0)) { continue; }
      if(src_section->addr < bin->base_vaddr) { continue; }
      String8 name = str8f(arena, "%S,%S", segname, sectname);
      U64 voff_first = src_section->addr - bin->base_vaddr;
      U64 voff_opl   = voff_first + src_section->size;

      // rjf: file range; zero-fill sections, and sections without backing file
      // data (e.g. the section headers dsymutil copies into dSYM files), have none
      U64 foff_first = 0;
      U64 foff_opl   = 0;
      {
        String8 file_data = macho_data_from_section(data, bin, src_section);
        if(file_data.size != 0)
        {
          foff_first = src_section->offset;
          foff_opl   = foff_first + file_data.size;
        }
      }

      // rjf: make rdi section if not empty
      if(name.size != 0 || voff_opl != voff_first || foff_opl != foff_first)
      {
        RDIM_BinarySection *dst_section = rdim_binary_section_list_push(arena, &result);
        dst_section->name       = name;
        dst_section->flags      = flags;
        dst_section->voff_first = voff_first;
        dst_section->voff_opl   = voff_opl;
        dst_section->foff_first = foff_first;
        dst_section->foff_opl   = foff_opl;
      }
    }
  }
  return result;
}
