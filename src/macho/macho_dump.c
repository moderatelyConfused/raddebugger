// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

internal String8
macho_dump_string_from_version_nibbles(Arena *arena, U32 v)
{
  // NOTE: X.Y.Z encoded as xxxx.yy.zz nibbles
  String8 result = str8f(arena, "%u.%u.%u", (v >> 16) & 0xffff, (v >> 8) & 0xff, v & 0xff);
  return result;
}

internal String8List
macho_dump(Arena *arena, String8 data, MACHO_Bin *bin, MACHO_DumpSubsetFlags flags)
{
  String8List out = {0};

  //////////////////////////////
  //- rjf: header
  //
  if(flags & MACHO_DumpSubsetFlag_Header)
  {
    str8_list_pushf(arena, &out, "// MACH-O HEADER\n\n");
    if(bin->is_fat)
    {
      str8_list_pushf(arena, &out, "  fat binary:    %I64u slices; selected cputype=0x%x cpusubtype=0x%x at [0x%I64x, 0x%I64x)\n",
                      bin->fat_arch_count, bin->fat_arch.cputype, bin->fat_arch.cpusubtype, bin->slice_range.min, bin->slice_range.max);
    }
    if(!bin->is_valid)
    {
      str8_list_pushf(arena, &out, "  (no 64-bit Mach-O header found)\n\n");
    }
    else
    {
      MACHO_Header64 *hdr = &bin->hdr;
      str8_list_pushf(arena, &out, "  magic:         0x%08x\n", hdr->magic);
      str8_list_pushf(arena, &out, "  cputype:       0x%08x (%S)\n", hdr->cputype, macho_string_from_cputype(hdr->cputype));
      str8_list_pushf(arena, &out, "  cpusubtype:    0x%08x\n", hdr->cpusubtype);
      str8_list_pushf(arena, &out, "  filetype:      0x%x (%S)\n", hdr->filetype, macho_string_from_filetype(hdr->filetype));
      str8_list_pushf(arena, &out, "  ncmds:         %u\n", hdr->ncmds);
      str8_list_pushf(arena, &out, "  sizeofcmds:    %u\n", hdr->sizeofcmds);
      str8_list_pushf(arena, &out, "  flags:         0x%08x%s\n", hdr->flags, bin->is_pie ? " (PIE)" : "");
      str8_list_pushf(arena, &out, "  base vaddr:    0x%I64x\n", bin->base_vaddr);
      if(bin->has_entry_point)
      {
        str8_list_pushf(arena, &out, "  entry voff:    0x%I64x\n", bin->entry_point_voff);
      }
      if(bin->has_uuid)
      {
        str8_list_pushf(arena, &out, "  uuid:          %08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X\n",
                        bin->uuid.data1, bin->uuid.data2, bin->uuid.data3,
                        bin->uuid.data4[0], bin->uuid.data4[1], bin->uuid.data4[2], bin->uuid.data4[3],
                        bin->uuid.data4[4], bin->uuid.data4[5], bin->uuid.data4[6], bin->uuid.data4[7]);
      }
      if(bin->dylib_install_name.size != 0)
      {
        str8_list_pushf(arena, &out, "  install name:  %S\n", bin->dylib_install_name);
      }
      if(bin->dylinker_name.size != 0)
      {
        str8_list_pushf(arena, &out, "  dylinker:      %S\n", bin->dylinker_name);
      }
      if(bin->has_build_version)
      {
        Temp scratch = scratch_begin(&arena, 1);
        str8_list_pushf(arena, &out, "  platform:      %S, minos %S, sdk %S\n",
                        macho_string_from_platform(bin->build_version.platform),
                        macho_dump_string_from_version_nibbles(scratch.arena, bin->build_version.minos),
                        macho_dump_string_from_version_nibbles(scratch.arena, bin->build_version.sdk));
        scratch_end(scratch);
      }
      if(bin->has_version_min)
      {
        Temp scratch = scratch_begin(&arena, 1);
        str8_list_pushf(arena, &out, "  version min:   %S, sdk %S\n",
                        macho_dump_string_from_version_nibbles(scratch.arena, bin->version_min.version),
                        macho_dump_string_from_version_nibbles(scratch.arena, bin->version_min.sdk));
        scratch_end(scratch);
      }
      str8_list_pushf(arena, &out, "  dwarf:         %s\n", macho_is_dwarf_present_from_bin(bin) ? "present (__DWARF,__debug_info)" : "absent");
      str8_list_pushf(arena, &out, "  debug map:     %s (%I64u N_OSO stabs)\n", bin->has_debug_map ? "present" : "absent", bin->oso_count);
      str8_list_pushf(arena, &out, "\n");
    }
  }

  //////////////////////////////
  //- rjf: load commands
  //
  if(bin->is_valid && (flags & MACHO_DumpSubsetFlag_LoadCommands))
  {
    str8_list_pushf(arena, &out, "// MACH-O LOAD COMMANDS\n\n");
    for EachIndex(idx, bin->load_commands.count)
    {
      MACHO_LoadCommandInfo *lc = &bin->load_commands.v[idx];
      String8 name = macho_string_from_load_command_type(lc->cmd);
      if(name.size == 0)
      {
        name = str8f(arena, "0x%x", lc->cmd);
      }
      str8_list_pushf(arena, &out, "  [%I64u] %S (off 0x%I64x, size %u)\n", idx, name, lc->off, lc->cmdsize);
    }
    for EachNode(n, String8Node, bin->dylibs.first)
    {
      str8_list_pushf(arena, &out, "  dylib: %S\n", n->string);
    }
    for EachNode(n, String8Node, bin->rpaths.first)
    {
      str8_list_pushf(arena, &out, "  rpath: %S\n", n->string);
    }
    str8_list_pushf(arena, &out, "\n");
  }

  //////////////////////////////
  //- rjf: segments & sections
  //
  if(bin->is_valid && (flags & MACHO_DumpSubsetFlag_Segments))
  {
    str8_list_pushf(arena, &out, "// MACH-O SEGMENTS\n\n");
    for EachIndex(seg_idx, bin->segments.count)
    {
      MACHO_Segment *segment = &bin->segments.v[seg_idx];
      MACHO_SegmentCommand64 *cmd = &segment->cmd;
      str8_list_pushf(arena, &out, "  segment[%I64u] \"%S\": vmaddr=0x%I64x vmsize=0x%I64x fileoff=0x%I64x filesize=0x%I64x maxprot=%c%c%c initprot=%c%c%c nsects=%u flags=0x%x\n",
                      seg_idx, macho_string_from_name16(cmd->segname),
                      cmd->vmaddr, cmd->vmsize, cmd->fileoff, cmd->filesize,
                      (cmd->maxprot  & MACHO_VM_PROT_READ)    ? 'r' : '-',
                      (cmd->maxprot  & MACHO_VM_PROT_WRITE)   ? 'w' : '-',
                      (cmd->maxprot  & MACHO_VM_PROT_EXECUTE) ? 'x' : '-',
                      (cmd->initprot & MACHO_VM_PROT_READ)    ? 'r' : '-',
                      (cmd->initprot & MACHO_VM_PROT_WRITE)   ? 'w' : '-',
                      (cmd->initprot & MACHO_VM_PROT_EXECUTE) ? 'x' : '-',
                      cmd->nsects, cmd->flags);
      for EachIndex(idx, segment->section_count)
      {
        U64 sect_idx = segment->first_section_idx + idx;
        MACHO_Section64 *section = &bin->sections.v[sect_idx];
        String8 type_name = macho_string_from_section_type(section->flags);
        if(type_name.size == 0)
        {
          type_name = str8f(arena, "0x%x", section->flags & MACHO_SECTION_TYPE);
        }
        str8_list_pushf(arena, &out, "    section[%I64u] \"%S,%S\": addr=0x%I64x size=0x%I64x offset=0x%x align=2^%u reloff=0x%x nreloc=%u type=%S attrs=0x%08x\n",
                        sect_idx + 1,
                        macho_string_from_name16(section->segname),
                        macho_string_from_name16(section->sectname),
                        section->addr, section->size, section->offset, section->align,
                        section->reloff, section->nreloc, type_name, section->flags & MACHO_SECTION_ATTRIBUTES);
      }
    }
    str8_list_pushf(arena, &out, "\n");
  }

  //////////////////////////////
  //- rjf: symbols
  //
  if(bin->is_valid && (flags & MACHO_DumpSubsetFlag_Symbols))
  {
    str8_list_pushf(arena, &out, "// MACH-O SYMBOLS\n\n");
    if(!bin->has_symtab)
    {
      str8_list_pushf(arena, &out, "  (no LC_SYMTAB)\n");
    }
    else
    {
      str8_list_pushf(arena, &out, "  symoff=0x%x nsyms=%u stroff=0x%x strsize=%u\n", bin->symtab.symoff, bin->symtab.nsyms, bin->symtab.stroff, bin->symtab.strsize);
      if(bin->has_dysymtab)
      {
        str8_list_pushf(arena, &out, "  local=[%u, +%u) extdef=[%u, +%u) undef=[%u, +%u)\n",
                        bin->dysymtab.ilocalsym, bin->dysymtab.nlocalsym,
                        bin->dysymtab.iextdefsym, bin->dysymtab.nextdefsym,
                        bin->dysymtab.iundefsym, bin->dysymtab.nundefsym);
      }
      for EachIndex(idx, bin->symbols.count)
      {
        MACHO_NList64 *nlist = &bin->symbols.v[idx];
        String8 name = macho_name_from_nlist(bin, nlist);
        if(nlist->n_type & MACHO_N_STAB)
        {
          String8 stab_name = macho_string_from_stab_type(nlist->n_type);
          if(stab_name.size == 0)
          {
            stab_name = str8f(arena, "stab 0x%02x", nlist->n_type);
          }
          str8_list_pushf(arena, &out, "  [%I64u] %S n_sect=%u n_desc=0x%04x n_value=0x%I64x \"%S\"\n",
                          idx, stab_name, nlist->n_sect, nlist->n_desc, nlist->n_value, name);
        }
        else
        {
          String8 type_name = str8_lit("?");
          switch(nlist->n_type & MACHO_N_TYPE)
          {
            default:{}break;
            case MACHO_N_UNDF:{type_name = str8_lit("N_UNDF");}break;
            case MACHO_N_ABS: {type_name = str8_lit("N_ABS");}break;
            case MACHO_N_SECT:{type_name = str8_lit("N_SECT");}break;
            case MACHO_N_PBUD:{type_name = str8_lit("N_PBUD");}break;
            case MACHO_N_INDR:{type_name = str8_lit("N_INDR");}break;
          }
          str8_list_pushf(arena, &out, "  [%I64u] %S%s%s n_sect=%u n_desc=0x%04x n_value=0x%I64x \"%S\"\n",
                          idx, type_name,
                          (nlist->n_type & MACHO_N_EXT)  ? " N_EXT"  : "",
                          (nlist->n_type & MACHO_N_PEXT) ? " N_PEXT" : "",
                          nlist->n_sect, nlist->n_desc, nlist->n_value, name);
        }
      }
    }
    str8_list_pushf(arena, &out, "\n");
  }

  //////////////////////////////
  //- rjf: function starts
  //
  if(bin->is_valid && (flags & MACHO_DumpSubsetFlag_FunctionStarts))
  {
    str8_list_pushf(arena, &out, "// MACH-O FUNCTION STARTS\n\n");
    if(!bin->has_function_starts)
    {
      str8_list_pushf(arena, &out, "  (no LC_FUNCTION_STARTS)\n");
    }
    else
    {
      str8_list_pushf(arena, &out, "  data=[0x%I64x, 0x%I64x) count=%I64u (voffs relative to base vaddr 0x%I64x)\n",
                      bin->function_starts_range.min, bin->function_starts_range.max, bin->function_starts.count, bin->base_vaddr);
      for EachIndex(idx, bin->function_starts.count)
      {
        str8_list_pushf(arena, &out, "  [%I64u] 0x%I64x\n", idx, bin->function_starts.v[idx]);
      }
    }
    str8_list_pushf(arena, &out, "\n");
  }

  return out;
}
