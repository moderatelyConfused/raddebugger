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

////////////////////////////////
//~ rjf: Symbol Table -> RDI (images without DWARF)
//
// Images without a `__DWARF` segment (the dyld shared cache's libSystem,
// libsystem_kernel, libdyld, ...; stripped executables) still carry their
// defined symbols in `LC_SYMTAB`. Like the CodeView converter's public
// symbols (`CV_SymKind_PUB32`), each defined symbol in a code section becomes
// a procedure with one root scope. A procedure ends at the next symbol of the
// same section, at the next `LC_FUNCTION_STARTS` entry (shared-cache images
// keep their function starts but not their local symbols, so an exported
// function would otherwise swallow the unnamed locals after it), or at the
// end of its section. Names drop the one leading underscore of the C ABI
// (`_write` -> `write`); the symbol itself is kept as the link name.

internal int
m2r_symtab_proc_compare(M2R_SymtabProc *a, M2R_SymtabProc *b)
{
  int result = 0;
  if(0){}
  else if(a->vaddr < b->vaddr)         { result = -1; }
  else if(a->vaddr > b->vaddr)         { result = +1; }
  else if(a->nlist_idx < b->nlist_idx) { result = -1; }
  else if(a->nlist_idx > b->nlist_idx) { result = +1; }
  return result;
}

internal RDIM_BakeParams
m2r_bake_params_from_macho_symtab(Arena *arena, M2R_SymtabConvertParams *params)
{
  Temp scratch = scratch_begin(&arena, 1);
  MACHO_Bin *bin = params->bin;
  String8 data = params->data;
  U64 base_vaddr = bin->base_vaddr;
  Arch arch = arch_from_macho_cputype(bin->hdr.cputype);

  //- rjf: binary sections
  RDIM_BinarySectionList binary_sections = m2r_rdi_binary_sections_from_macho(arena, data, bin);

  //- rjf: top-level info (see `d2r_convert` for the guid's role in staleness checks)
  RDIM_TopLevelInfo top_level_info = {0};
  {
    top_level_info.arch     = rdi_arch_from_arch(arch);
    top_level_info.exe_name = params->exe_name;
    top_level_info.exe_hash = rdi_hash(data.str, data.size);
    for EachNode(n, RDIM_BinarySectionNode, binary_sections.first)
    {
      top_level_info.voff_max = Max(top_level_info.voff_max, n->v.voff_opl);
    }
    if(!params->deterministic)
    {
      top_level_info.producer_name = str8_lit(BUILD_TITLE_STRING_LITERAL);
    }
    if(bin->has_uuid)
    {
      MemoryCopy(&top_level_info.guid, &bin->uuid, sizeof(top_level_info.guid));
    }
  }

  //- rjf: gather defined symbols in code sections (nlist n_sect is 1-based)
  U64 proc_count = 0;
  M2R_SymtabProc *procs = push_array_no_zero(scratch.arena, M2R_SymtabProc, bin->symbols.count);
  for EachIndex(idx, bin->symbols.count)
  {
    MACHO_NList64 *nlist = &bin->symbols.v[idx];
    if(nlist->n_type & MACHO_N_STAB)                                   { continue; }
    if((nlist->n_type & MACHO_N_TYPE) != MACHO_N_SECT)                 { continue; }
    if(nlist->n_sect == 0 || nlist->n_sect > bin->sections.count)      { continue; }
    if(nlist->n_strx == 0 || nlist->n_strx >= bin->string_table.size)  { continue; }
    MACHO_Section64 *section = &bin->sections.v[nlist->n_sect - 1];
    if(!(section->flags & (MACHO_S_ATTR_PURE_INSTRUCTIONS|MACHO_S_ATTR_SOME_INSTRUCTIONS))) { continue; }
    if(section->addr < base_vaddr)                                                           { continue; }
    if(nlist->n_value < section->addr || nlist->n_value >= section->addr + section->size)    { continue; }
    M2R_SymtabProc *proc = &procs[proc_count];
    proc->vaddr       = nlist->n_value;
    proc->section_idx = nlist->n_sect - 1;
    proc->nlist_idx   = idx;
    proc_count += 1;
  }
  quick_sort(procs, proc_count, sizeof(M2R_SymtabProc), m2r_symtab_proc_compare);

  //- rjf: one unit holds all procedures
  RDIM_UnitChunkList units = {0};
  if(proc_count != 0)
  {
    RDIM_Unit *unit = rdim_unit_chunk_list_push(arena, &units, 1);
    unit->unit_name = str8_skip_last_slash(params->exe_name);
    unit->language  = RDI_Language_NULL;

    //- rjf: build procedures & root scopes
    U64 function_starts_cursor = 0;
    for EachIndex(idx, proc_count)
    {
      M2R_SymtabProc *proc = &procs[idx];
      MACHO_NList64 *nlist = &bin->symbols.v[proc->nlist_idx];
      MACHO_Section64 *section = &bin->sections.v[proc->section_idx];
      U64 voff_first = proc->vaddr - base_vaddr;

      // rjf: end at the section end, the next symbol in this section, or the next function start
      U64 voff_opl = section->addr + section->size - base_vaddr;
      for(U64 next_idx = idx+1; next_idx < proc_count; next_idx += 1)
      {
        if(procs[next_idx].vaddr != proc->vaddr)
        {
          if(procs[next_idx].section_idx == proc->section_idx)
          {
            voff_opl = Min(voff_opl, procs[next_idx].vaddr - base_vaddr);
          }
          break;
        }
      }
      for(; function_starts_cursor < bin->function_starts.count && bin->function_starts.v[function_starts_cursor] <= voff_first; function_starts_cursor += 1){}
      if(function_starts_cursor < bin->function_starts.count)
      {
        voff_opl = Min(voff_opl, bin->function_starts.v[function_starts_cursor]);
      }
      if(voff_opl <= voff_first)
      {
        voff_opl = voff_first + 1;
      }

      // rjf: names
      String8 link_name = macho_name_from_nlist(bin, nlist);
      String8 name = link_name;
      if(name.size > 1 && name.str[0] == '_')
      {
        name = str8_skip(name, 1);
      }

      // rjf: build symbol / scope
      RDIM_Symbol *symbol = rdim_symbol_chunk_list_push(arena, &unit->procedures, proc_count);
      symbol->name      = name;
      symbol->link_name = link_name;
      symbol->is_extern = !!(nlist->n_type & MACHO_N_EXT);
      RDIM_Scope *scope = rdim_scope_chunk_list_push(arena, &unit->scopes, proc_count);
      scope->symbol = symbol;
      RDIM_Rng1U64 range = {voff_first, voff_opl};
      rdim_scope_push_voff_range(arena, &unit->scopes, scope, range);
      symbol->root_scope = scope;
    }
  }

  //- rjf: fill result
  RDIM_BakeParams result = {0};
  {
    result.subset_flags    = params->subset_flags;
    result.top_level_info  = top_level_info;
    result.binary_sections = binary_sections;
    result.units           = units;
  }
  scratch_end(scratch);
  return result;
}
