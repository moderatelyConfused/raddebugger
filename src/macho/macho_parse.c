// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

////////////////////////////////
//~ rjf: Fat (Universal) Binary Parsing

internal MACHO_FatArchArray
macho_fat_arch_array_from_data(Arena *arena, String8 data)
{
  MACHO_FatArchArray result = {0};
  MACHO_FatHeader fat_hdr = {0};
  if(str8_deserial_read_struct(data, 0, &fat_hdr) == sizeof(fat_hdr))
  {
    U32 magic     = from_be_u32(fat_hdr.magic);
    U32 nfat_arch = from_be_u32(fat_hdr.nfat_arch);
    B32 is_64     = (magic == MACHO_FAT_MAGIC_64);
    if((magic == MACHO_FAT_MAGIC || is_64) &&
       0 < nfat_arch && nfat_arch < MACHO_FAT_ARCH_MAX_COUNT)
    {
      U64 entry_size = is_64 ? sizeof(MACHO_FatArch64) : sizeof(MACHO_FatArch);
      U64 entries_off = sizeof(fat_hdr);
      if(entries_off + nfat_arch*entry_size <= data.size)
      {
        result.count = nfat_arch;
        result.v = push_array(arena, MACHO_FatArch64, nfat_arch);
        for EachIndex(idx, nfat_arch)
        {
          U64 off = entries_off + idx*entry_size;
          MACHO_FatArch64 *dst = &result.v[idx];
          if(is_64)
          {
            MACHO_FatArch64 raw = {0};
            str8_deserial_read_struct(data, off, &raw);
            dst->cputype    = from_be_u32(raw.cputype);
            dst->cpusubtype = from_be_u32(raw.cpusubtype);
            dst->offset     = from_be_u64(raw.offset);
            dst->size       = from_be_u64(raw.size);
            dst->align      = from_be_u32(raw.align);
          }
          else
          {
            MACHO_FatArch raw = {0};
            str8_deserial_read_struct(data, off, &raw);
            dst->cputype    = from_be_u32(raw.cputype);
            dst->cpusubtype = from_be_u32(raw.cpusubtype);
            dst->offset     = from_be_u32(raw.offset);
            dst->size       = from_be_u32(raw.size);
            dst->align      = from_be_u32(raw.align);
          }
        }
      }
    }
  }
  return result;
}

internal U64
macho_fat_arch_idx_from_array(MACHO_FatArchArray *arches)
{
  U64 best_idx = arches->count;
  S32 best_score = 0;
  for EachIndex(idx, arches->count)
  {
    MACHO_FatArch64 *arch = &arches->v[idx];
    S32 score = 0;
    if(arch->cputype == MACHO_CPU_TYPE_ARM64)
    {
      U32 subtype = (arch->cpusubtype & ~MACHO_CPU_SUBTYPE_MASK);
      score = (subtype == MACHO_CPU_SUBTYPE_ARM64E) ? 3 : 4;
    }
    else if(arch->cputype == MACHO_CPU_TYPE_X86_64)
    {
      score = 2;
    }
    else
    {
      score = 1;
    }
    if(score > best_score)
    {
      best_score = score;
      best_idx = idx;
    }
  }
  return best_idx;
}

////////////////////////////////
//~ rjf: Top-Level Binary Parsing

internal String8
macho_lc_str_from_command(String8 slice, MACHO_LoadCommandInfo *lc, U32 str_off)
{
  // NOTE: `lc_str` strings are stored inside the load command, at an offset
  // relative to the start of the command, NUL-terminated within cmdsize.
  String8 result = {0};
  if(str_off < lc->cmdsize && lc->off + lc->cmdsize <= slice.size)
  {
    U8 *base = slice.str + lc->off;
    result = str8_cstring_capped(base + str_off, base + lc->cmdsize);
  }
  return result;
}

internal Rng1U64
macho_linkedit_data_range_from_command(String8 slice, MACHO_LoadCommandInfo *lc)
{
  Rng1U64 result = {0};
  MACHO_LinkeditDataCommand led = {0};
  if(lc->cmdsize >= sizeof(led) && str8_deserial_read_struct(slice, lc->off, &led) == sizeof(led))
  {
    U64 min = ClampTop((U64)led.dataoff, slice.size);
    U64 max = ClampTop((U64)led.dataoff + (U64)led.datasize, slice.size);
    result = r1u64(min, max);
  }
  return result;
}

internal MACHO_Bin
macho_bin_from_data(Arena *arena, String8 data)
{
  MACHO_Bin bin = {0};

  //////////////////////////////
  //- rjf: select slice (thin file, or best slice of a fat file)
  //
  String8 slice = data;
  Rng1U64 slice_range = r1u64(0, data.size);
  {
    Temp scratch = scratch_begin(&arena, 1);
    MACHO_FatArchArray fat_arches = macho_fat_arch_array_from_data(scratch.arena, data);
    if(fat_arches.count != 0)
    {
      bin.is_fat = 1;
      bin.fat_arch_count = fat_arches.count;
      U64 idx = macho_fat_arch_idx_from_array(&fat_arches);
      slice = str8_zero();
      slice_range = r1u64(0, 0);
      if(idx < fat_arches.count)
      {
        MACHO_FatArch64 *arch = &fat_arches.v[idx];
        if(arch->offset <= data.size && arch->size <= data.size - arch->offset)
        {
          bin.fat_arch = *arch;
          slice_range = r1u64(arch->offset, arch->offset + arch->size);
          slice = str8_substr(data, slice_range);
        }
      }
    }
    scratch_end(scratch);
  }
  bin.slice = slice;
  bin.slice_range = slice_range;

  //////////////////////////////
  //- rjf: parse header
  //
  MACHO_Header64 hdr = {0};
  if(str8_deserial_read_struct(slice, 0, &hdr) == sizeof(hdr) &&
     hdr.magic == MACHO_MH_MAGIC_64)
  {
    bin.is_valid = 1;
    bin.hdr = hdr;
    bin.is_pie = !!(hdr.flags & MACHO_MH_PIE);
  }

  //////////////////////////////
  //- rjf: gather load commands (bounds-checked; never trust cmdsize)
  //
  if(bin.is_valid)
  {
    U64 cmds_off = sizeof(MACHO_Header64);
    U64 cmds_opl = ClampTop(cmds_off + (U64)hdr.sizeofcmds, slice.size);

    //- rjf: pass 1: count valid commands, segments, sections
    U64 lc_count = 0;
    U64 seg_count = 0;
    U64 sect_count = 0;
    for(U64 off = cmds_off, idx = 0; idx < hdr.ncmds; idx += 1)
    {
      MACHO_LoadCommand lc = {0};
      if(str8_deserial_read_struct(slice, off, &lc) != sizeof(lc)) { break; }
      if(lc.cmdsize < sizeof(lc) || lc.cmdsize > cmds_opl - off) { break; }
      lc_count += 1;
      if(lc.cmd == MACHO_LC_SEGMENT_64 && lc.cmdsize >= sizeof(MACHO_SegmentCommand64))
      {
        MACHO_SegmentCommand64 seg = {0};
        str8_deserial_read_struct(slice, off, &seg);
        U64 max_nsects = (lc.cmdsize - sizeof(MACHO_SegmentCommand64)) / sizeof(MACHO_Section64);
        seg_count += 1;
        sect_count += Min((U64)seg.nsects, max_nsects);
      }
      off += lc.cmdsize;
    }

    //- rjf: allocate
    bin.load_commands.count = lc_count;
    bin.load_commands.v = push_array(arena, MACHO_LoadCommandInfo, lc_count);
    bin.segments.count = seg_count;
    bin.segments.v = push_array(arena, MACHO_Segment, seg_count);
    bin.sections.count = sect_count;
    bin.sections.v = push_array(arena, MACHO_Section64, sect_count);

    //- rjf: pass 2: fill
    B32 has_thread_pc = 0;
    U64 thread_pc = 0;
    {
      U64 lc_idx = 0;
      U64 seg_idx = 0;
      U64 sect_idx = 0;
      for(U64 off = cmds_off; lc_idx < lc_count; lc_idx += 1)
      {
        MACHO_LoadCommand lc = {0};
        str8_deserial_read_struct(slice, off, &lc);
        MACHO_LoadCommandInfo *lc_info = &bin.load_commands.v[lc_idx];
        lc_info->cmd     = lc.cmd;
        lc_info->cmdsize = lc.cmdsize;
        lc_info->off     = off;
        switch(lc.cmd)
        {
          default:{}break;

          //- rjf: segments & sections
          case MACHO_LC_SEGMENT_64:
          if(lc.cmdsize >= sizeof(MACHO_SegmentCommand64))
          {
            MACHO_Segment *segment = &bin.segments.v[seg_idx];
            seg_idx += 1;
            str8_deserial_read_struct(slice, off, &segment->cmd);
            U64 max_nsects = (lc.cmdsize - sizeof(MACHO_SegmentCommand64)) / sizeof(MACHO_Section64);
            U64 nsects = Min((U64)segment->cmd.nsects, max_nsects);
            segment->first_section_idx = sect_idx;
            segment->section_count = nsects;
            for EachIndex(idx, nsects)
            {
              U64 sect_off = off + sizeof(MACHO_SegmentCommand64) + idx*sizeof(MACHO_Section64);
              str8_deserial_read_struct(slice, sect_off, &bin.sections.v[sect_idx]);
              sect_idx += 1;
            }
            if(!bin.has_text_segment &&
               str8_match(macho_string_from_name16(segment->cmd.segname), str8_lit("__TEXT"), 0))
            {
              bin.has_text_segment = 1;
              bin.text_segment_idx = seg_idx - 1;
              bin.base_vaddr = segment->cmd.vmaddr;
            }
          }break;

          //- rjf: symbol tables
          case MACHO_LC_SYMTAB:
          if(lc.cmdsize >= sizeof(MACHO_SymtabCommand) && !bin.has_symtab)
          {
            bin.has_symtab = 1;
            str8_deserial_read_struct(slice, off, &bin.symtab);
          }break;
          case MACHO_LC_DYSYMTAB:
          if(lc.cmdsize >= sizeof(MACHO_DysymtabCommand) && !bin.has_dysymtab)
          {
            bin.has_dysymtab = 1;
            str8_deserial_read_struct(slice, off, &bin.dysymtab);
          }break;

          //- rjf: uuid
          case MACHO_LC_UUID:
          if(lc.cmdsize >= sizeof(MACHO_UUIDCommand))
          {
            MACHO_UUIDCommand uuid_cmd = {0};
            str8_deserial_read_struct(slice, off, &uuid_cmd);
            bin.has_uuid = 1;
            // NOTE: Mach-O UUIDs are raw 16-byte arrays. we unpack them such
            // that the canonical (big-endian) textual form matches what Apple
            // tools print, when formatted via data1/data2/data3/data4.
            bin.uuid.data1 = ((U32)uuid_cmd.uuid[0] << 24) | ((U32)uuid_cmd.uuid[1] << 16) | ((U32)uuid_cmd.uuid[2] << 8) | (U32)uuid_cmd.uuid[3];
            bin.uuid.data2 = (U16)(((U16)uuid_cmd.uuid[4] << 8) | (U16)uuid_cmd.uuid[5]);
            bin.uuid.data3 = (U16)(((U16)uuid_cmd.uuid[6] << 8) | (U16)uuid_cmd.uuid[7]);
            MemoryCopy(bin.uuid.data4, &uuid_cmd.uuid[8], 8);
          }break;

          //- rjf: entry points
          case MACHO_LC_MAIN:
          if(lc.cmdsize >= sizeof(MACHO_EntryPointCommand))
          {
            MACHO_EntryPointCommand ep = {0};
            str8_deserial_read_struct(slice, off, &ep);
            // NOTE: entryoff is an offset from the start of __TEXT, which is
            // where the mach_header lives (file offset 0 / base_vaddr).
            bin.has_entry_point = 1;
            bin.entry_point_voff = ep.entryoff;
            bin.entry_point_stacksize = ep.stacksize;
          }break;
          case MACHO_LC_UNIXTHREAD:
          case MACHO_LC_THREAD:
          {
            // NOTE: thread commands hold one or more {flavor, count, state[count]} blocks.
            U64 cursor = off + sizeof(MACHO_ThreadCommand);
            U64 opl = off + lc.cmdsize;
            for(;cursor + 8 <= opl;)
            {
              U32 flavor = 0;
              U32 count = 0;
              str8_deserial_read_struct(slice, cursor, &flavor);
              str8_deserial_read_struct(slice, cursor + 4, &count);
              cursor += 8;
              U64 state_size = (U64)count * 4;
              if(state_size > opl - cursor) { break; }
              U64 pc_idx = max_U64;
              if(flavor == MACHO_ARM_THREAD_STATE64 && hdr.cputype == MACHO_CPU_TYPE_ARM64)
              {
                pc_idx = MACHO_ARM_THREAD_STATE64_PC_INDEX;
              }
              else if(flavor == MACHO_x86_THREAD_STATE64 && hdr.cputype == MACHO_CPU_TYPE_X86_64)
              {
                pc_idx = MACHO_x86_THREAD_STATE64_RIP_INDEX;
              }
              if(pc_idx != max_U64 && (pc_idx + 1) * 8 <= state_size && !has_thread_pc)
              {
                U64 pc = 0;
                if(str8_deserial_read_struct(slice, cursor + pc_idx*8, &pc) == sizeof(pc))
                {
                  has_thread_pc = 1;
                  thread_pc = pc;
                }
              }
              cursor += state_size;
            }
          }break;

          //- rjf: __LINKEDIT data
          case MACHO_LC_FUNCTION_STARTS:
          {
            bin.has_function_starts = 1;
            bin.function_starts_range = macho_linkedit_data_range_from_command(slice, lc_info);
          }break;
          case MACHO_LC_DATA_IN_CODE:        {bin.data_in_code_range       = macho_linkedit_data_range_from_command(slice, lc_info);}break;
          case MACHO_LC_CODE_SIGNATURE:      {bin.code_signature_range     = macho_linkedit_data_range_from_command(slice, lc_info);}break;
          case MACHO_LC_DYLD_EXPORTS_TRIE:   {bin.exports_trie_range       = macho_linkedit_data_range_from_command(slice, lc_info);}break;
          case MACHO_LC_DYLD_CHAINED_FIXUPS: {bin.chained_fixups_range     = macho_linkedit_data_range_from_command(slice, lc_info);}break;
          case MACHO_LC_SEGMENT_SPLIT_INFO:  {bin.segment_split_info_range = macho_linkedit_data_range_from_command(slice, lc_info);}break;
          case MACHO_LC_DYLD_INFO:
          case MACHO_LC_DYLD_INFO_ONLY:
          if(lc.cmdsize >= sizeof(MACHO_DyldInfoCommand))
          {
            bin.has_dyld_info = 1;
            str8_deserial_read_struct(slice, off, &bin.dyld_info);
          }break;

          //- rjf: dylibs / dylinker / rpaths
          case MACHO_LC_ID_DYLIB:
          if(lc.cmdsize >= sizeof(MACHO_DylibCommand))
          {
            MACHO_DylibCommand dylib = {0};
            str8_deserial_read_struct(slice, off, &dylib);
            bin.dylib_install_name = macho_lc_str_from_command(slice, lc_info, dylib.name_offset);
          }break;
          case MACHO_LC_LOAD_DYLIB:
          case MACHO_LC_LOAD_WEAK_DYLIB:
          case MACHO_LC_REEXPORT_DYLIB:
          case MACHO_LC_LOAD_UPWARD_DYLIB:
          case MACHO_LC_LAZY_LOAD_DYLIB:
          if(lc.cmdsize >= sizeof(MACHO_DylibCommand))
          {
            MACHO_DylibCommand dylib = {0};
            str8_deserial_read_struct(slice, off, &dylib);
            String8 name = macho_lc_str_from_command(slice, lc_info, dylib.name_offset);
            str8_list_push(arena, &bin.dylibs, name);
          }break;
          case MACHO_LC_LOAD_DYLINKER:
          if(lc.cmdsize >= sizeof(MACHO_DylinkerCommand))
          {
            MACHO_DylinkerCommand dylinker = {0};
            str8_deserial_read_struct(slice, off, &dylinker);
            bin.dylinker_name = macho_lc_str_from_command(slice, lc_info, dylinker.name_offset);
          }break;
          case MACHO_LC_RPATH:
          if(lc.cmdsize >= sizeof(MACHO_RpathCommand))
          {
            MACHO_RpathCommand rpath = {0};
            str8_deserial_read_struct(slice, off, &rpath);
            String8 path = macho_lc_str_from_command(slice, lc_info, rpath.path_offset);
            str8_list_push(arena, &bin.rpaths, path);
          }break;

          //- rjf: versions
          case MACHO_LC_BUILD_VERSION:
          if(lc.cmdsize >= sizeof(MACHO_BuildVersionCommand))
          {
            bin.has_build_version = 1;
            str8_deserial_read_struct(slice, off, &bin.build_version);
          }break;
          case MACHO_LC_VERSION_MIN_MACOSX:
          case MACHO_LC_VERSION_MIN_IPHONEOS:
          case MACHO_LC_VERSION_MIN_TVOS:
          case MACHO_LC_VERSION_MIN_WATCHOS:
          if(lc.cmdsize >= sizeof(MACHO_VersionMinCommand))
          {
            bin.has_version_min = 1;
            str8_deserial_read_struct(slice, off, &bin.version_min);
          }break;
          case MACHO_LC_SOURCE_VERSION:
          if(lc.cmdsize >= sizeof(MACHO_SourceVersionCommand))
          {
            MACHO_SourceVersionCommand sv = {0};
            str8_deserial_read_struct(slice, off, &sv);
            bin.has_source_version = 1;
            bin.source_version = sv.version;
          }break;
        }
        off += lc.cmdsize;
      }
    }

    //- rjf: no __TEXT segment (e.g. MH_OBJECT) => base is the first segment's vmaddr
    if(!bin.has_text_segment && bin.segments.count != 0)
    {
      bin.base_vaddr = bin.segments.v[0].cmd.vmaddr;
    }

    //- rjf: LC_UNIXTHREAD pc => entry point voff
    if(!bin.has_entry_point && has_thread_pc && thread_pc >= bin.base_vaddr)
    {
      bin.has_entry_point = 1;
      bin.entry_point_voff = thread_pc - bin.base_vaddr;
    }

    //- rjf: unpack symbol table
    if(bin.has_symtab)
    {
      U64 symoff = bin.symtab.symoff;
      U64 nsyms = bin.symtab.nsyms;
      if(symoff > slice.size)
      {
        nsyms = 0;
      }
      else
      {
        nsyms = Min(nsyms, (slice.size - symoff) / sizeof(MACHO_NList64));
      }
      bin.symbols.count = nsyms;
      bin.symbols.v = push_array_no_zero(arena, MACHO_NList64, nsyms);
      if(nsyms != 0)
      {
        MemoryCopy(bin.symbols.v, slice.str + symoff, nsyms*sizeof(MACHO_NList64));
      }
      bin.string_table = str8_substr(slice, r1u64(bin.symtab.stroff, (U64)bin.symtab.stroff + (U64)bin.symtab.strsize));
      for EachIndex(idx, nsyms)
      {
        MACHO_NList64 *nlist = &bin.symbols.v[idx];
        if(nlist->n_type & MACHO_N_STAB)
        {
          bin.stab_count += 1;
          if(nlist->n_type == MACHO_N_OSO)
          {
            bin.oso_count += 1;
          }
        }
      }
      bin.has_debug_map = (bin.oso_count != 0);
    }

    //- rjf: decode function starts (ULEB128 deltas; first is relative to __TEXT start; 0 terminates)
    if(bin.has_function_starts)
    {
      String8 fs_data = str8_substr(slice, bin.function_starts_range);
      U64 count = 0;
      for(U64 off = 0; off < fs_data.size;)
      {
        U64 delta = 0;
        U64 delta_size = str8_deserial_read_uleb128(fs_data, off, &delta);
        if(delta_size == 0 || delta == 0) { break; }
        off += delta_size;
        count += 1;
      }
      bin.function_starts.count = count;
      bin.function_starts.v = push_array_no_zero(arena, U64, count);
      U64 voff = 0;
      U64 idx = 0;
      for(U64 off = 0; off < fs_data.size && idx < count;)
      {
        U64 delta = 0;
        U64 delta_size = str8_deserial_read_uleb128(fs_data, off, &delta);
        if(delta_size == 0 || delta == 0) { break; }
        off += delta_size;
        voff += delta;
        bin.function_starts.v[idx] = voff;
        idx += 1;
      }
      bin.function_starts.count = idx;
    }
  }

  return bin;
}

////////////////////////////////
//~ rjf: Segment / Section Helpers

internal String8
macho_slice_from_data(String8 data, MACHO_Bin *bin)
{
  String8 result = str8_substr(data, bin->slice_range);
  return result;
}

internal MACHO_Segment *
macho_segment_from_name(MACHO_Bin *bin, String8 segname)
{
  MACHO_Segment *result = 0;
  for EachIndex(idx, bin->segments.count)
  {
    MACHO_Segment *segment = &bin->segments.v[idx];
    if(str8_match(macho_string_from_name16(segment->cmd.segname), segname, 0))
    {
      result = segment;
      break;
    }
  }
  return result;
}

internal MACHO_Segment *
macho_segment_from_section_idx(MACHO_Bin *bin, U64 section_idx)
{
  MACHO_Segment *result = 0;
  for EachIndex(idx, bin->segments.count)
  {
    MACHO_Segment *segment = &bin->segments.v[idx];
    if(segment->first_section_idx <= section_idx &&
       section_idx < segment->first_section_idx + segment->section_count)
    {
      result = segment;
      break;
    }
  }
  return result;
}

internal MACHO_Section64 *
macho_section_from_name(MACHO_Bin *bin, String8 segname, String8 sectname)
{
  MACHO_Section64 *result = 0;
  for EachIndex(idx, bin->sections.count)
  {
    MACHO_Section64 *section = &bin->sections.v[idx];
    if(str8_match(macho_string_from_name16(section->segname), segname, 0) &&
       str8_match(macho_string_from_name16(section->sectname), sectname, 0))
    {
      result = section;
      break;
    }
  }
  return result;
}

internal U64
macho_section_idx_from_section(MACHO_Bin *bin, MACHO_Section64 *section)
{
  U64 result = bin->sections.count;
  if(bin->sections.v <= section && section < bin->sections.v + bin->sections.count)
  {
    result = (U64)(section - bin->sections.v);
  }
  return result;
}

internal String8
macho_name_from_section(Arena *arena, MACHO_Section64 *section)
{
  String8 result = str8f(arena, "%S,%S", macho_string_from_name16(section->segname), macho_string_from_name16(section->sectname));
  return result;
}

internal String8
macho_data_from_segment(String8 data, MACHO_Bin *bin, MACHO_Segment *segment)
{
  String8 slice = macho_slice_from_data(data, bin);
  String8 result = {0};
  if(segment->cmd.fileoff <= slice.size)
  {
    U64 size = Min(segment->cmd.filesize, slice.size - segment->cmd.fileoff);
    result = str8_substr(slice, r1u64(segment->cmd.fileoff, segment->cmd.fileoff + size));
  }
  return result;
}

internal String8
macho_data_from_section(String8 data, MACHO_Bin *bin, MACHO_Section64 *section)
{
  String8 result = {0};
  if(!macho_section_type_is_zerofill(section->flags))
  {
    String8 slice = macho_slice_from_data(data, bin);
    U64 off = section->offset;
    U64 size = section->size;
    B32 has_file_data = (off <= slice.size && size <= slice.size - off);

    // rjf: check that the section's file range is covered by its owning
    // segment's file range. in dSYM files the non-__DWARF sections are copied
    // from the executable with offset 0 and no backing file data.
    U64 section_idx = macho_section_idx_from_section(bin, section);
    MACHO_Segment *segment = (section_idx < bin->sections.count) ? macho_segment_from_section_idx(bin, section_idx) : 0;
    if(has_file_data && segment != 0)
    {
      U64 seg_min = segment->cmd.fileoff;
      U64 seg_max = segment->cmd.fileoff + segment->cmd.filesize;
      if(segment->cmd.filesize == 0 || off < seg_min || off + size > seg_max)
      {
        has_file_data = 0;
      }
    }

    if(has_file_data)
    {
      result = str8_substr(slice, r1u64(off, off + size));
    }
  }
  return result;
}

////////////////////////////////
//~ rjf: Symbol Helpers

internal String8
macho_name_from_nlist(MACHO_Bin *bin, MACHO_NList64 *nlist)
{
  String8 result = {0};
  if(nlist->n_strx < bin->string_table.size)
  {
    result = str8_cstring_capped(bin->string_table.str + nlist->n_strx, bin->string_table.str + bin->string_table.size);
  }
  return result;
}

internal B32
macho_is_dwarf_present_from_bin(MACHO_Bin *bin)
{
  MACHO_Section64 *debug_info = macho_section_from_name(bin, str8_lit("__DWARF"), str8_lit("__debug_info"));
  B32 result = (debug_info != 0 && debug_info->size != 0);
  return result;
}

internal B32
macho_has_debug_map_from_bin(MACHO_Bin *bin)
{
  return bin->has_debug_map;
}

////////////////////////////////
//~ rjf: dSYM Paths

internal String8
macho_dsym_path_from_exe_path(Arena *arena, String8 exe_path)
{
  String8 exe_name = str8_skip_last_slash(exe_path);
  String8 result = str8f(arena, "%S.dSYM/Contents/Resources/DWARF/%S", exe_path, exe_name);
  return result;
}
