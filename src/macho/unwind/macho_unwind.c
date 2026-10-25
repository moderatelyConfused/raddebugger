// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

////////////////////////////////
//~ rjf: Lookups

internal B32
macho_uwnd_encoding_from_voff(MACHO_UWND_ModuleUnwindInfo *info, U64 voff, U32 *encoding_out)
{
  B32 found = 0;
  U32 encoding = 0;
  String8 data = info->unwind_info_data;
  MACHO_UWND_SectionHeader hdr = {0};
  if(data.size >= sizeof(hdr))
  {
    MemoryCopy(&hdr, data.str, sizeof(hdr));
  }
  if(hdr.version == MACHO_UWND_SECTION_VERSION && hdr.index_count != 0 && voff <= max_U32 &&
     (U64)hdr.index_section_offset + (U64)hdr.index_count*sizeof(MACHO_UWND_IndexEntry) <= data.size)
  {
    //- rjf: first level: binary search the index for the last entry whose function offset <= voff
    U64 index_off = hdr.index_section_offset;
    U64 index_count = hdr.index_count;
    MACHO_UWND_IndexEntry first_entry = {0};
    MemoryCopy(&first_entry, data.str + index_off, sizeof(first_entry));
    if(voff >= first_entry.function_offset)
    {
      U64 lo = 0;
      U64 hi = index_count - 1;
      for(;lo < hi;)
      {
        U64 mid = lo + (hi - lo + 1)/2;
        MACHO_UWND_IndexEntry mid_entry = {0};
        MemoryCopy(&mid_entry, data.str + index_off + mid*sizeof(mid_entry), sizeof(mid_entry));
        if(mid_entry.function_offset <= voff) { lo = mid; }
        else                                  { hi = mid - 1; }
      }
      MACHO_UWND_IndexEntry index_entry = {0};
      MemoryCopy(&index_entry, data.str + index_off + lo*sizeof(index_entry), sizeof(index_entry));
      
      //- rjf: second level page (the final index entry is a sentinel with no page)
      U64 page_off = index_entry.second_level_pages_section_offset;
      if(page_off != 0 && page_off + sizeof(U32) <= data.size)
      {
        U32 kind = 0;
        MemoryCopy(&kind, data.str + page_off, sizeof(kind));
        
        //- rjf: regular page
        if(kind == MACHO_UWND_SECOND_LEVEL_REGULAR && page_off + sizeof(MACHO_UWND_RegularPageHeader) <= data.size)
        {
          MACHO_UWND_RegularPageHeader page = {0};
          MemoryCopy(&page, data.str + page_off, sizeof(page));
          U64 entries_off = page_off + page.entry_page_offset;
          U64 entries_count = page.entry_count;
          if(entries_count != 0 && entries_off + entries_count*sizeof(MACHO_UWND_RegularEntry) <= data.size)
          {
            U64 elo = 0;
            U64 ehi = entries_count - 1;
            for(;elo < ehi;)
            {
              U64 mid = elo + (ehi - elo + 1)/2;
              MACHO_UWND_RegularEntry mid_entry = {0};
              MemoryCopy(&mid_entry, data.str + entries_off + mid*sizeof(mid_entry), sizeof(mid_entry));
              if(mid_entry.function_offset <= voff) { elo = mid; }
              else                                  { ehi = mid - 1; }
            }
            MACHO_UWND_RegularEntry entry = {0};
            MemoryCopy(&entry, data.str + entries_off + elo*sizeof(entry), sizeof(entry));
            if(entry.function_offset <= voff)
            {
              encoding = entry.encoding;
              found = 1;
            }
          }
        }
        
        //- rjf: compressed page
        else if(kind == MACHO_UWND_SECOND_LEVEL_COMPRESSED && page_off + sizeof(MACHO_UWND_CompressedPageHeader) <= data.size)
        {
          MACHO_UWND_CompressedPageHeader page = {0};
          MemoryCopy(&page, data.str + page_off, sizeof(page));
          U64 entries_off = page_off + page.entry_page_offset;
          U64 entries_count = page.entry_count;
          if(entries_count != 0 && entries_off + entries_count*sizeof(U32) <= data.size)
          {
            U64 elo = 0;
            U64 ehi = entries_count - 1;
            for(;elo < ehi;)
            {
              U64 mid = elo + (ehi - elo + 1)/2;
              U32 mid_entry = 0;
              MemoryCopy(&mid_entry, data.str + entries_off + mid*sizeof(mid_entry), sizeof(mid_entry));
              U64 mid_function_off = (U64)index_entry.function_offset + MACHO_UWND_COMPRESSED_ENTRY_FUNC_OFFSET(mid_entry);
              if(mid_function_off <= voff) { elo = mid; }
              else                         { ehi = mid - 1; }
            }
            U32 entry = 0;
            MemoryCopy(&entry, data.str + entries_off + elo*sizeof(entry), sizeof(entry));
            U64 function_off = (U64)index_entry.function_offset + MACHO_UWND_COMPRESSED_ENTRY_FUNC_OFFSET(entry);
            if(function_off <= voff)
            {
              U32 encoding_idx = MACHO_UWND_COMPRESSED_ENTRY_ENCODING_INDEX(entry);
              if(encoding_idx < hdr.common_encodings_array_count)
              {
                U64 off = (U64)hdr.common_encodings_array_section_offset + (U64)encoding_idx*sizeof(U32);
                if(off + sizeof(U32) <= data.size)
                {
                  MemoryCopy(&encoding, data.str + off, sizeof(encoding));
                  found = 1;
                }
              }
              else
              {
                U64 local_idx = encoding_idx - hdr.common_encodings_array_count;
                U64 off = page_off + page.encodings_page_offset + local_idx*sizeof(U32);
                if(local_idx < page.encodings_count && off + sizeof(U32) <= data.size)
                {
                  MemoryCopy(&encoding, data.str + off, sizeof(encoding));
                  found = 1;
                }
              }
            }
          }
        }
      }
    }
  }
  if(encoding_out != 0)
  {
    encoding_out[0] = encoding;
  }
  return found;
}

////////////////////////////////
//~ rjf: Unwinding Abstraction Implementation

internal UWND_StepResult
macho_uwnd_step(Arch arch, MemoryMap *memory_map, UWND_ModuleInfo *module_info, U64 tls_vaddr, void *regs, U64 *cfa_out)
{
  UWND_StepResult result = {UWND_StepStatus_Error};
  MACHO_UWND_ModuleUnwindInfo *info = (MACHO_UWND_ModuleUnwindInfo *)module_info->unwind_info;
  if(info == 0 || arch != Arch_arm64)
  {
    return result;
  }
  ARM64_RegBlock *r = (ARM64_RegBlock *)regs;
  U64 pc = r->pc;
  U64 voff = pc - module_info->base_vaddr;
  
  //- rjf: look up the compact encoding for this pc
  U32 encoding = 0;
  B32 found = macho_uwnd_encoding_from_voff(info, voff, &encoding);
  U32 mode = encoding & MACHO_UWND_ARM64_MODE_MASK;
  
  //- rjf: DWARF mode -> use the FDE referenced from the encoding
  B32 done = 0;
  if(found && mode == MACHO_UWND_ARM64_MODE_DWARF && dim_1u64(info->eh_frame_vaddr_range) != 0)
  {
    U64 fde_vaddr = info->eh_frame_vaddr_range.min + (encoding & MACHO_UWND_ARM64_DWARF_SECTION_OFFSET);
    if(contains_1u64(info->eh_frame_vaddr_range, fde_vaddr))
    {
      result = eh_uwnd_step_from_fde_vaddr(arch, memory_map, &info->ptr_ctx, fde_vaddr, tls_vaddr, regs, cfa_out);
      done = (result.status != UWND_StepStatus_Error);
    }
  }
  
  //- rjf: frame / frameless modes
  if(!done && found && (mode == MACHO_UWND_ARM64_MODE_FRAME || mode == MACHO_UWND_ARM64_MODE_FRAMELESS))
  {
    // rjf: count saved register slots
    U64 pair_bits[] =
    {
      MACHO_UWND_ARM64_FRAME_X19_X20_PAIR,
      MACHO_UWND_ARM64_FRAME_X21_X22_PAIR,
      MACHO_UWND_ARM64_FRAME_X23_X24_PAIR,
      MACHO_UWND_ARM64_FRAME_X25_X26_PAIR,
      MACHO_UWND_ARM64_FRAME_X27_X28_PAIR,
      MACHO_UWND_ARM64_FRAME_D8_D9_PAIR,
      MACHO_UWND_ARM64_FRAME_D10_D11_PAIR,
      MACHO_UWND_ARM64_FRAME_D12_D13_PAIR,
      MACHO_UWND_ARM64_FRAME_D14_D15_PAIR,
    };
    U64 saved_slot_count = 0;
    for EachIndex(idx, ArrayCount(pair_bits))
    {
      if(encoding & pair_bits[idx]) { saved_slot_count += 2; }
    }
    
    // rjf: determine the frame layout
    //
    // FRAME:     [fp] = caller fp, [fp+8] = return address, callee-saved pairs below fp
    //            (x19 at fp-8, x20 at fp-16, x21 at fp-24, ...), cfa = fp + 16
    // FRAMELESS: cfa = sp + stack_size, return address in lr, callee-saved pairs
    //            at the top of the frame (x19 at cfa-8, x20 at cfa-16, ...)
    //
    U64 cfa = 0;
    U64 saved_top = 0;
    if(mode == MACHO_UWND_ARM64_MODE_FRAME)
    {
      U64 fp = r->fp;
      if(fp == 0)
      {
        return result;
      }
      cfa = fp + 16;
      saved_top = fp;
    }
    else
    {
      U64 stack_size = ((encoding & MACHO_UWND_ARM64_FRAMELESS_STACK_SIZE_MASK) >> 12) * 16;
      cfa = r->sp + stack_size;
      saved_top = cfa;
    }
    
    // rjf: read the whole region of interest at once
    U64 saved_lo = saved_top - saved_slot_count*8;
    Rng1U64 read_range = r1u64(saved_lo, (mode == MACHO_UWND_ARM64_MODE_FRAME) ? cfa : saved_top);
    U64 read_size = dim_1u64(read_range);
    if(read_size != 0)
    {
      Temp scratch = scratch_begin(0, 0);
      U8 *buffer = push_array(scratch.arena, U8, read_size);
      if(memory_map_read(memory_map, read_range, buffer) != read_size)
      {
        result.status = UWND_StepStatus_FailedMemoryRead;
        result.missed_read_vaddr_range = read_range;
        scratch_end(scratch);
        return result;
      }
#define MACHO_UWND_SLOT(vaddr) (*(U64 *)(buffer + ((vaddr) - read_range.min)))
      
      // rjf: restore callee-saved registers
      U64 loc = saved_top - 8;
      U64 *gpr_pairs[5][2] = {{&r->x19, &r->x20}, {&r->x21, &r->x22}, {&r->x23, &r->x24}, {&r->x25, &r->x26}, {&r->x27, &r->x28}};
      for EachIndex(idx, 5)
      {
        if(encoding & pair_bits[idx])
        {
          *gpr_pairs[idx][0] = MACHO_UWND_SLOT(loc); loc -= 8;
          *gpr_pairs[idx][1] = MACHO_UWND_SLOT(loc); loc -= 8;
        }
      }
      U128 *vec_pairs[4][2] = {{&r->v8, &r->v9}, {&r->v10, &r->v11}, {&r->v12, &r->v13}, {&r->v14, &r->v15}};
      for EachIndex(idx, 4)
      {
        if(encoding & pair_bits[5+idx])
        {
          vec_pairs[idx][0]->u64[0] = MACHO_UWND_SLOT(loc); loc -= 8;
          vec_pairs[idx][1]->u64[0] = MACHO_UWND_SLOT(loc); loc -= 8;
        }
      }
      
      // rjf: restore fp / lr / pc / sp
      if(mode == MACHO_UWND_ARM64_MODE_FRAME)
      {
        U64 fp = r->fp;
        U64 caller_fp = MACHO_UWND_SLOT(fp);
        U64 return_address = MACHO_UWND_SLOT(fp+8);
        r->fp = caller_fp;
        r->lr = return_address;
        r->pc = return_address;
      }
      else
      {
        r->pc = r->lr;
      }
#undef MACHO_UWND_SLOT
      scratch_end(scratch);
    }
    else
    {
      r->pc = r->lr;
    }
    r->sp = cfa;
    if(cfa_out) { cfa_out[0] = cfa; }
    result.status = UWND_StepStatus_Good;
    done = 1;
  }
  
  //- rjf: no usable compact info -> frame pointer chain
  if(!done)
  {
    result = uwnd_step_moduleless(arch, memory_map, regs);
    if(result.status == UWND_StepStatus_Good && cfa_out)
    {
      cfa_out[0] = r->sp;
    }
  }
  return result;
}
