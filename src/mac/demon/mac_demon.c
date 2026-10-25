// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

////////////////////////////////
//~ rjf: Build Checks

#if __DARWIN_OPAQUE_ARM_THREAD_STATE64
# error The macOS demon layer does not support the opaque (arm64e / pointer-authenticated) thread state layout; build for plain arm64.
#endif

////////////////////////////////
//~ rjf: Memory R/W Helpers

internal U64
mac_dmn_read(mach_port_t task, Rng1U64 range, void *dst)
{
  U64 size = dim_1u64(range);
  if(size == 0 || task == MACH_PORT_NULL)
  {
    return 0;
  }
  
  //- rjf: fast path: whole range
  {
    mach_vm_size_t out_size = 0;
    kern_return_t kr = mach_vm_read_overwrite(task, (mach_vm_address_t)range.min, (mach_vm_size_t)size, (mach_vm_address_t)dst, &out_size);
    if(kr == KERN_SUCCESS && out_size == size)
    {
      return size;
    }
  }
  
  //- rjf: slow path: page by page, stop at the first unreadable page
  U64 page = mac_dmn_state->page_size;
  U64 total = 0;
  for(U64 cursor = range.min; cursor < range.max;)
  {
    U64 next = AlignDownPow2(cursor, page) + page;
    if(next > range.max || next < cursor) { next = range.max; }
    U64 chunk = next - cursor;
    mach_vm_size_t out_size = 0;
    kern_return_t kr = mach_vm_read_overwrite(task, (mach_vm_address_t)cursor, (mach_vm_size_t)chunk, (mach_vm_address_t)((U8 *)dst + total), &out_size);
    if(kr != KERN_SUCCESS || out_size == 0)
    {
      break;
    }
    total += out_size;
    cursor += out_size;
    if(out_size < chunk)
    {
      break;
    }
  }
  return total;
}

internal B32
mac_dmn_write(mach_port_t task, Rng1U64 range, void *src)
{
  U64 size = dim_1u64(range);
  if(size == 0) { return 1; }
  if(task == MACH_PORT_NULL) { return 0; }
  
  //- rjf: fast path: directly writable memory
  {
    kern_return_t kr = mach_vm_write(task, (mach_vm_address_t)range.min, (vm_offset_t)src, (mach_msg_type_number_t)size);
    if(kr == KERN_SUCCESS)
    {
      return 1;
    }
  }
  
  //- rjf: slow path: page by page; temporarily make the page writable
  // (copy-on-write), which is required to patch code pages (breakpoints).
  B32 good = 1;
  U64 page = mac_dmn_state->page_size;
  U64 src_off = 0;
  for(U64 cursor = range.min; cursor < range.max;)
  {
    U64 page_base = AlignDownPow2(cursor, page);
    U64 next = page_base + page;
    if(next > range.max || next < cursor) { next = range.max; }
    U64 chunk = next - cursor;
    
    // rjf: query original protection of this page
    vm_prot_t original_protection = VM_PROT_READ|VM_PROT_EXECUTE;
    {
      mach_vm_address_t region_addr = (mach_vm_address_t)page_base;
      mach_vm_size_t region_size = 0;
      vm_region_basic_info_data_64_t info = {0};
      mach_msg_type_number_t info_count = VM_REGION_BASIC_INFO_COUNT_64;
      mach_port_t object_name = MACH_PORT_NULL;
      kern_return_t kr = mach_vm_region(task, &region_addr, &region_size, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &info_count, &object_name);
      if(kr == KERN_SUCCESS && region_addr <= page_base)
      {
        original_protection = info.protection;
      }
      if(object_name != MACH_PORT_NULL)
      {
        mach_port_deallocate(mach_task_self(), object_name);
      }
    }
    
    // rjf: make writable, write, restore
    kern_return_t protect_kr = mach_vm_protect(task, (mach_vm_address_t)page_base, (mach_vm_size_t)page, 0, VM_PROT_READ|VM_PROT_WRITE|VM_PROT_COPY);
    if(protect_kr != KERN_SUCCESS)
    {
      good = 0;
      break;
    }
    kern_return_t write_kr = mach_vm_write(task, (mach_vm_address_t)cursor, (vm_offset_t)((U8 *)src + src_off), (mach_msg_type_number_t)chunk);
    mach_vm_protect(task, (mach_vm_address_t)page_base, (mach_vm_size_t)page, 0, original_protection);
    if(write_kr != KERN_SUCCESS)
    {
      good = 0;
      break;
    }
    cursor = next;
    src_off += chunk;
  }
  return good;
}

internal String8
mac_dmn_read_string_capped(Arena *arena, mach_port_t task, U64 base_vaddr, U64 cap_size)
{
  String8 result = {0};
  if(base_vaddr == 0)
  {
    return result;
  }
  Temp scratch = scratch_begin(&arena, 1);
  U8 *buffer = push_array_no_zero(scratch.arena, U8, cap_size+1);
  U64 total = 0;
  B32 done = 0;
  // rjf: read in chunks up to page boundaries so that a string at the end of a
  // mapping does not fail as a whole
  U64 page = mac_dmn_state->page_size;
  for(U64 cursor = base_vaddr; !done && total < cap_size;)
  {
    U64 next = AlignDownPow2(cursor, page) + page;
    U64 chunk = Min(next - cursor, cap_size - total);
    U64 read_size = mac_dmn_read(task, r1u64(cursor, cursor + chunk), buffer + total);
    if(read_size == 0)
    {
      break;
    }
    for(U64 idx = 0; idx < read_size; idx += 1)
    {
      if(buffer[total + idx] == 0)
      {
        total += idx;
        done = 1;
        break;
      }
    }
    if(!done)
    {
      total += read_size;
      cursor += read_size;
      if(read_size < chunk)
      {
        break;
      }
    }
  }
  result = push_str8_copy(arena, str8(buffer, total));
  scratch_end(scratch);
  return result;
}

////////////////////////////////
//~ rjf: In-Memory Mach-O Parsing Helpers

internal MAC_DMN_ImageInfo
mac_dmn_image_info_from_header_vaddr(mach_port_t task, U64 header_vaddr)
{
  MAC_DMN_ImageInfo info = {0};
  struct mach_header_64 hdr = {0};
  if(mac_dmn_read_struct(task, header_vaddr, &hdr) != sizeof(hdr) || hdr.magic != MH_MAGIC_64)
  {
    return info;
  }
  if(hdr.sizeofcmds == 0 || hdr.sizeofcmds > MB(16))
  {
    return info;
  }
  Temp scratch = scratch_begin(0, 0);
  U64 cmds_size = hdr.sizeofcmds;
  U8 *cmds = push_array_no_zero(scratch.arena, U8, cmds_size);
  if(mac_dmn_read(task, r1u64(header_vaddr + sizeof(hdr), header_vaddr + sizeof(hdr) + cmds_size), cmds) == cmds_size)
  {
    info.valid        = 1;
    info.cputype      = hdr.cputype;
    info.filetype     = hdr.filetype;
    info.header_vaddr = header_vaddr;
    
    //- rjf: scan load commands
    U64 text_vmaddr = 0;
    U64 text_vmsize = 0;
    B32 got_text = 0;
    U64 linkedit_vmaddr = 0;
    U64 linkedit_fileoff = 0;
    B32 got_linkedit = 0;
    struct symtab_command symtab = {0};
    B32 got_symtab = 0;
    U64 entry_voff = 0;
    U64 unixthread_pc = 0;
    B32 got_unixthread = 0;
    typedef struct SegRange SegRange;
    struct SegRange { U64 min; U64 max; };
    SegRange seg_ranges[64] = {0};
    U64 seg_ranges_count = 0;
    Rng1U64 text_range = {0};
    Rng1U64 unwind_range = {0};
    Rng1U64 eh_frame_range = {0};
    U64 thread_vars_vaddr = 0;
    for(U64 off = 0, cmd_idx = 0; cmd_idx < hdr.ncmds && off + sizeof(struct load_command) <= cmds_size; cmd_idx += 1)
    {
      struct load_command lc = {0};
      MemoryCopy(&lc, cmds + off, sizeof(lc));
      if(lc.cmdsize < sizeof(lc) || off + lc.cmdsize > cmds_size)
      {
        break;
      }
      switch(lc.cmd)
      {
        default:{}break;
        case LC_SEGMENT_64:
        if(lc.cmdsize >= sizeof(struct segment_command_64))
        {
          struct segment_command_64 seg = {0};
          MemoryCopy(&seg, cmds + off, sizeof(seg));
          B32 is_pagezero = (MemoryMatch(seg.segname, "__PAGEZERO", 11));
          B32 is_text     = (MemoryMatch(seg.segname, "__TEXT", 7));
          B32 is_linkedit = (MemoryMatch(seg.segname, "__LINKEDIT", 11));
          if(is_text)
          {
            text_vmaddr = seg.vmaddr;
            text_vmsize = seg.vmsize;
            got_text = 1;
          }
          if(is_linkedit)
          {
            linkedit_vmaddr = seg.vmaddr;
            linkedit_fileoff = seg.fileoff;
            got_linkedit = 1;
          }
          if(!is_pagezero && seg.vmsize != 0 && seg_ranges_count < ArrayCount(seg_ranges))
          {
            seg_ranges[seg_ranges_count].min = seg.vmaddr;
            seg_ranges[seg_ranges_count].max = seg.vmaddr + seg.vmsize;
            seg_ranges_count += 1;
          }
          U64 sect_off = off + sizeof(seg);
          for(U32 sect_idx = 0;
              sect_idx < seg.nsects && sect_off + sizeof(struct section_64) <= off + lc.cmdsize;
              sect_idx += 1, sect_off += sizeof(struct section_64))
          {
            struct section_64 sect = {0};
            MemoryCopy(&sect, cmds + sect_off, sizeof(sect));
            Rng1U64 sect_range = r1u64(sect.addr, sect.addr + sect.size);
            if(is_text)
            {
              if(MemoryMatch(sect.sectname, "__text", 7))        { text_range = sect_range; }
              if(MemoryMatch(sect.sectname, "__unwind_info", 14)){ unwind_range = sect_range; }
              if(MemoryMatch(sect.sectname, "__eh_frame", 11))   { eh_frame_range = sect_range; }
            }
            if((sect.flags & SECTION_TYPE) == S_THREAD_LOCAL_VARIABLES && thread_vars_vaddr == 0)
            {
              thread_vars_vaddr = sect.addr;
            }
          }
        }break;
        case LC_UUID:
        if(lc.cmdsize >= sizeof(struct uuid_command))
        {
          struct uuid_command uuid = {0};
          MemoryCopy(&uuid, cmds + off, sizeof(uuid));
          MemoryCopy(info.uuid.v, uuid.uuid, sizeof(info.uuid.v));
        }break;
        case LC_MAIN:
        if(lc.cmdsize >= sizeof(struct entry_point_command))
        {
          struct entry_point_command ep = {0};
          MemoryCopy(&ep, cmds + off, sizeof(ep));
          entry_voff = ep.entryoff;
        }break;
        case LC_UNIXTHREAD:
        {
          // rjf: cmd, cmdsize, flavor, count, then arm_thread_state64 (x[29], fp, lr, sp, pc, ...)
          U64 pc_off = 16 + 29*8 + 3*8;
          if(lc.cmdsize >= pc_off + 8)
          {
            MemoryCopy(&unixthread_pc, cmds + off + pc_off, sizeof(unixthread_pc));
            got_unixthread = 1;
          }
        }break;
        case LC_SYMTAB:
        if(lc.cmdsize >= sizeof(struct symtab_command))
        {
          MemoryCopy(&symtab, cmds + off, sizeof(symtab));
          got_symtab = 1;
        }break;
      }
      off += lc.cmdsize;
    }
    
    //- rjf: compute slide & slid ranges
    U64 slide = got_text ? (header_vaddr - text_vmaddr) : 0;
    info.slide = slide;
    if(got_unixthread && entry_voff == 0 && got_text && unixthread_pc >= text_vmaddr)
    {
      entry_voff = unixthread_pc - text_vmaddr;
    }
    info.entry_point_voff = entry_voff;
    if(dim_1u64(text_range) != 0)     { info.text_vaddr_range = shift_1u64(text_range, slide); }
    if(dim_1u64(unwind_range) != 0)   { info.unwind_info_vaddr_range = shift_1u64(unwind_range, slide); }
    if(dim_1u64(eh_frame_range) != 0) { info.eh_frame_vaddr_range = shift_1u64(eh_frame_range, slide); }
    if(thread_vars_vaddr != 0)        { info.thread_vars_vaddr = thread_vars_vaddr + slide; }
    
    //- rjf: compute virtual size. images inside the dyld shared cache have
    // their segments scattered across the cache, in which case we only
    // claim the __TEXT segment, so that modules do not overlap.
    {
      U64 lo = max_U64;
      U64 hi = 0;
      for EachIndex(idx, seg_ranges_count)
      {
        lo = Min(lo, seg_ranges[idx].min);
        hi = Max(hi, seg_ranges[idx].max);
      }
      B32 contiguous = (seg_ranges_count != 0);
      for EachIndex(idx, seg_ranges_count)
      {
        // rjf: every segment must start within 64MB of the end of some other segment or of the image start
        B32 anchored = (seg_ranges[idx].min == lo);
        for EachIndex(idx2, seg_ranges_count)
        {
          if(idx2 != idx && seg_ranges[idx2].max <= seg_ranges[idx].min && seg_ranges[idx].min - seg_ranges[idx2].max <= MB(64))
          {
            anchored = 1;
          }
        }
        if(!anchored)
        {
          contiguous = 0;
        }
      }
      if(contiguous && hi > lo && got_text && lo == text_vmaddr)
      {
        info.vsize = hi - lo;
      }
      else
      {
        info.vsize = text_vmsize;
      }
      if(info.vsize == 0)
      {
        info.vsize = mac_dmn_state->page_size;
      }
    }
    
    //- rjf: symbol table addresses (in-memory)
    if(got_symtab && got_linkedit && symtab.symoff >= linkedit_fileoff && symtab.stroff >= linkedit_fileoff)
    {
      info.symtab_vaddr = linkedit_vmaddr + slide + (symtab.symoff - linkedit_fileoff);
      info.symtab_count = symtab.nsyms;
      info.strtab_vaddr = linkedit_vmaddr + slide + (symtab.stroff - linkedit_fileoff);
      info.strtab_size  = symtab.strsize;
    }
  }
  scratch_end(scratch);
  return info;
}

internal U64
mac_dmn_symbol_vaddr_from_image(mach_port_t task, MAC_DMN_ImageInfo *image, String8 name)
{
  U64 result = 0;
  if(image->valid && image->symtab_vaddr != 0 && image->symtab_count != 0 && image->strtab_vaddr != 0 && image->strtab_size != 0)
  {
    Temp scratch = scratch_begin(0, 0);
    U64 sym_count = Min(image->symtab_count, (U64)(1<<22));
    U64 str_size = Min(image->strtab_size, MB(128));
    struct nlist_64 *syms = push_array_no_zero(scratch.arena, struct nlist_64, sym_count);
    U8 *strs = push_array_no_zero(scratch.arena, U8, str_size);
    U64 syms_read = mac_dmn_read(task, r1u64(image->symtab_vaddr, image->symtab_vaddr + sym_count*sizeof(struct nlist_64)), syms);
    U64 strs_read = mac_dmn_read(task, r1u64(image->strtab_vaddr, image->strtab_vaddr + str_size), strs);
    sym_count = syms_read / sizeof(struct nlist_64);
    for EachIndex(idx, sym_count)
    {
      struct nlist_64 *sym = &syms[idx];
      if((sym->n_type & N_STAB) != 0) { continue; }
      if((sym->n_type & N_TYPE) != N_SECT) { continue; }
      if(sym->n_un.n_strx >= strs_read) { continue; }
      String8 sym_name = str8_cstring_capped(strs + sym->n_un.n_strx, strs + strs_read);
      if(str8_match(sym_name, name, 0))
      {
        result = sym->n_value + image->slide;
        break;
      }
    }
    scratch_end(scratch);
  }
  return result;
}

internal U64
mac_dmn_image_header_vaddr_from_vaddr_scan(mach_port_t task, U64 vaddr, U32 filetype)
{
  U64 result = 0;
  U64 page = mac_dmn_state->page_size;
  U64 cursor = AlignDownPow2(vaddr, page);
  for(U64 idx = 0; idx < 8192 && cursor >= page; idx += 1, cursor -= page)
  {
    struct mach_header_64 hdr = {0};
    if(mac_dmn_read_struct(task, cursor, &hdr) == sizeof(hdr) &&
       hdr.magic == MH_MAGIC_64 &&
       (filetype == 0 || hdr.filetype == filetype))
    {
      result = cursor;
      break;
    }
  }
  return result;
}

////////////////////////////////
//~ rjf: Entity Functions

internal MAC_DMN_Entity *
mac_dmn_entity_alloc(MAC_DMN_EntityKind kind)
{
  MAC_DMN_Entity *entity = mac_dmn_state->free_entity;
  if(entity != 0)
  {
    SLLStackPop(mac_dmn_state->free_entity);
  }
  else
  {
    entity = push_array(mac_dmn_state->entities_arena, MAC_DMN_Entity, 1);
    mac_dmn_state->entities_count += 1;
  }
  U32 gen = entity->gen;
  MemoryZeroStruct(entity);
  entity->gen = gen + 1;
  entity->kind = kind;
  return entity;
}

internal void
mac_dmn_entity_release(MAC_DMN_Entity *entity)
{
  U32 gen = entity->gen + 1;
  MemoryZeroStruct(entity);
  entity->gen = gen;
  SLLStackPush(mac_dmn_state->free_entity, entity);
}

internal MAC_DMN_Process *
mac_dmn_process_alloc(pid_t pid, mach_port_t task, MAC_DMN_ProcessState state, B32 debug_subprocesses, B32 is_child)
{
  MAC_DMN_Process *process = &mac_dmn_entity_alloc(MAC_DMN_EntityKind_Process)->process;
  process->pid                = pid;
  process->task               = task;
  process->state              = state;
  process->debug_subprocesses = debug_subprocesses;
  process->is_child           = is_child;
  if(state != MAC_DMN_ProcessState_Normal)
  {
    mac_dmn_state->process_pending_creation += 1;
  }
  DLLPushBack(mac_dmn_state->first_process, mac_dmn_state->last_process, process);
  mac_dmn_state->process_count += 1;
  {
    U64 key = (U64)pid;
    U64 hash = u64_hash_from_str8(str8_struct(&key));
    U64 slot_idx = hash%mac_dmn_state->process_from_pid_slots_count;
    DLLPushBack_NP(mac_dmn_state->process_from_pid_slots[slot_idx].first, mac_dmn_state->process_from_pid_slots[slot_idx].last, process, pid_next, pid_prev);
  }
  
  // rjf: register exit notification
  {
    struct kevent kev = {0};
    EV_SET(&kev, (uintptr_t)pid, EVFILT_PROC, EV_ADD|EV_ENABLE|EV_ONESHOT, NOTE_EXIT|NOTE_EXITSTATUS, 0, 0);
    kevent(mac_dmn_state->kq, &kev, 1, 0, 0, 0);
  }
  return process;
}

internal void
mac_dmn_process_release(MAC_DMN_Process *process)
{
  // rjf: release threads
  for(MAC_DMN_Thread *thread = process->first_thread, *next = 0; thread != 0; thread = next)
  {
    next = thread->next;
    mac_dmn_thread_release(thread);
  }
  
  // rjf: unregister exit notification
  {
    struct kevent kev = {0};
    EV_SET(&kev, (uintptr_t)process->pid, EVFILT_PROC, EV_DELETE, 0, 0, 0);
    kevent(mac_dmn_state->kq, &kev, 1, 0, 0, 0);
  }
  
  // rjf: unlink
  DLLRemove(mac_dmn_state->first_process, mac_dmn_state->last_process, process);
  mac_dmn_state->process_count -= 1;
  if(process->state != MAC_DMN_ProcessState_Normal)
  {
    mac_dmn_state->process_pending_creation -= 1;
  }
  {
    U64 key = (U64)process->pid;
    U64 hash = u64_hash_from_str8(str8_struct(&key));
    U64 slot_idx = hash%mac_dmn_state->process_from_pid_slots_count;
    DLLRemove_NP(mac_dmn_state->process_from_pid_slots[slot_idx].first, mac_dmn_state->process_from_pid_slots[slot_idx].last, process, pid_next, pid_prev);
  }
  
  // rjf: release saved exception ports
  for EachIndex(idx, process->saved_exc_count)
  {
    if(process->saved_exc_ports[idx] != MACH_PORT_NULL)
    {
      mach_port_deallocate(mach_task_self(), process->saved_exc_ports[idx]);
    }
  }
  process->saved_exc_count = 0;
  
  // rjf: release task port & context
  if(process->task != MACH_PORT_NULL)
  {
    mach_port_deallocate(mach_task_self(), process->task);
    process->task = MACH_PORT_NULL;
  }
  if(process->ctx != 0)
  {
    process->ctx->ref_count -= 1;
    if(process->ctx->ref_count == 0)
    {
      for(MAC_DMN_Module *m = process->ctx->first_module, *next = 0; m != 0; m = next)
      {
        next = m->order_next;
        mac_dmn_entity_release((MAC_DMN_Entity *)m);
      }
      arena_release(process->ctx->arena);
      mac_dmn_entity_release((MAC_DMN_Entity *)process->ctx);
    }
  }
  mac_dmn_entity_release((MAC_DMN_Entity *)process);
}

internal MAC_DMN_Thread *
mac_dmn_thread_alloc(MAC_DMN_Process *process, mach_port_t port)
{
  MAC_DMN_ProcessCtx *ctx = process->ctx;
  
  // rjf: allocate reg block
  void *reg_block = 0;
  {
    if(ctx->free_reg_blocks.node_count)
    {
      String8Node *n = str8_list_pop_front(&ctx->free_reg_blocks);
      reg_block = n->string.str;
      str8_list_push_node(&ctx->free_reg_block_nodes, n);
    }
    else
    {
      ARCH_Info *arch_info = arch_info_from_arch(ctx->arch);
      reg_block = push_array(ctx->arena, U8, arch_info->reg_block_size);
    }
    MemoryZero(reg_block, arch_info_from_arch(ctx->arch)->reg_block_size);
  }
  
  // rjf: allocate thread
  MAC_DMN_Thread *thread = &mac_dmn_entity_alloc(MAC_DMN_EntityKind_Thread)->thread;
  thread->process   = process;
  thread->port      = port;
  thread->reg_block = reg_block;
  thread->state     = MAC_DMN_ThreadState_Stopped;
  
  // rjf: gather identifier info
  thread->tid = (U64)port;
  mac_dmn_thread_refresh_identifier_info(thread);
  
  // rjf: link
  DLLPushBack(process->first_thread, process->last_thread, thread);
  process->thread_count += 1;
  {
    U64 key = (U64)port;
    U64 hash = u64_hash_from_str8(str8_struct(&key));
    U64 slot_idx = hash%mac_dmn_state->thread_from_port_slots_count;
    DLLPushBack_NP(mac_dmn_state->thread_from_port_slots[slot_idx].first, mac_dmn_state->thread_from_port_slots[slot_idx].last, thread, port_next, port_prev);
  }
  return thread;
}

internal void
mac_dmn_thread_release(MAC_DMN_Thread *thread)
{
  MAC_DMN_Process *process = thread->process;
  MAC_DMN_ProcessCtx *ctx = process->ctx;
  
  // rjf: unlink
  DLLRemove(process->first_thread, process->last_thread, thread);
  process->thread_count -= 1;
  {
    U64 key = (U64)thread->port;
    U64 hash = u64_hash_from_str8(str8_struct(&key));
    U64 slot_idx = hash%mac_dmn_state->thread_from_port_slots_count;
    DLLRemove_NP(mac_dmn_state->thread_from_port_slots[slot_idx].first, mac_dmn_state->thread_from_port_slots[slot_idx].last, thread, port_next, port_prev);
  }
  
  // rjf: release reg block
  if(ctx != 0 && thread->reg_block != 0)
  {
    String8Node *reg_block_node = 0;
    if(ctx->free_reg_block_nodes.node_count)
    {
      reg_block_node = str8_list_pop_front(&ctx->free_reg_block_nodes);
    }
    else
    {
      reg_block_node = push_array(ctx->arena, String8Node, 1);
    }
    reg_block_node->string = str8(thread->reg_block, 0);
    str8_list_push_node(&ctx->free_reg_blocks, reg_block_node);
  }
  
  // rjf: release an unreplied exception's reply right
  if(thread->has_pending_exception && thread->pending_exception.head.msgh_remote_port != MACH_PORT_NULL)
  {
    mach_port_deallocate(mach_task_self(), thread->pending_exception.head.msgh_remote_port);
    thread->has_pending_exception = 0;
  }
  
  // rjf: release port
  if(thread->port != MACH_PORT_NULL)
  {
    mach_port_deallocate(mach_task_self(), thread->port);
  }
  mac_dmn_entity_release((MAC_DMN_Entity *)thread);
}

internal MAC_DMN_Module *
mac_dmn_module_from_base_vaddr(MAC_DMN_ProcessCtx *ctx, U64 base_vaddr)
{
  MAC_DMN_Module *result = 0;
  U64 hash = u64_hash_from_str8(str8_struct(&base_vaddr));
  U64 slot_idx = hash%ctx->module_slots_count;
  for(MAC_DMN_Module *m = ctx->module_slots[slot_idx].first; m != 0; m = m->hash_next)
  {
    if(m->base_vaddr == base_vaddr)
    {
      result = m;
      break;
    }
  }
  return result;
}

internal MAC_DMN_Module *
mac_dmn_module_alloc(MAC_DMN_ProcessCtx *ctx, U64 base_vaddr, U64 size, String8 path, U64 thread_vars_vaddr, B32 is_main, B32 is_dyld)
{
  MAC_DMN_Module *module = mac_dmn_module_from_base_vaddr(ctx, base_vaddr);
  if(module == 0)
  {
    module = &mac_dmn_entity_alloc(MAC_DMN_EntityKind_Module)->module;
    module->base_vaddr        = base_vaddr;
    module->size              = size;
    module->path              = push_str8_copy(ctx->arena, path);
    module->thread_vars_vaddr = thread_vars_vaddr;
    module->is_main           = is_main;
    module->is_dyld           = is_dyld;
    module->is_live           = 1;
    DLLPushBack_NP(ctx->first_module, ctx->last_module, module, order_next, order_prev);
    ctx->module_count += 1;
    U64 hash = u64_hash_from_str8(str8_struct(&base_vaddr));
    U64 slot_idx = hash%ctx->module_slots_count;
    DLLPushBack_NP(ctx->module_slots[slot_idx].first, ctx->module_slots[slot_idx].last, module, hash_next, hash_prev);
  }
  return module;
}

internal void
mac_dmn_module_release(MAC_DMN_ProcessCtx *ctx, MAC_DMN_Module *module)
{
  DLLRemove_NP(ctx->first_module, ctx->last_module, module, order_next, order_prev);
  ctx->module_count -= 1;
  {
    U64 hash = u64_hash_from_str8(str8_struct(&module->base_vaddr));
    U64 slot_idx = hash%ctx->module_slots_count;
    DLLRemove_NP(ctx->module_slots[slot_idx].first, ctx->module_slots[slot_idx].last, module, hash_next, hash_prev);
  }
  mac_dmn_entity_release((MAC_DMN_Entity *)module);
}

internal DMN_Handle
mac_dmn_handle_from_entity(MAC_DMN_Entity *entity)
{
  DMN_Handle handle = {0};
  if(entity != 0)
  {
    U64 index = (U64)(entity - mac_dmn_state->entities_base);
    if(index <= max_U32)
    {
      handle.u32[0] = (U32)index;
      handle.u32[1] = entity->gen;
    }
  }
  return handle;
}

internal DMN_Handle mac_dmn_handle_from_process(MAC_DMN_Process *process) { return mac_dmn_handle_from_entity((MAC_DMN_Entity *)process); }
internal DMN_Handle mac_dmn_handle_from_thread(MAC_DMN_Thread *thread)    { return mac_dmn_handle_from_entity((MAC_DMN_Entity *)thread); }
internal DMN_Handle mac_dmn_handle_from_module(MAC_DMN_Module *module)    { return mac_dmn_handle_from_entity((MAC_DMN_Entity *)module); }

internal MAC_DMN_Entity *
mac_dmn_entity_from_handle(DMN_Handle handle, MAC_DMN_EntityKind expected_kind)
{
  MAC_DMN_Entity *result = 0;
  U32 index = handle.u32[0];
  U32 gen = handle.u32[1];
  if(index < mac_dmn_state->entities_count && mac_dmn_state->entities_base[index].gen == gen)
  {
    if(mac_dmn_state->entities_base[index].kind == expected_kind)
    {
      result = &mac_dmn_state->entities_base[index];
    }
  }
  return result;
}

internal MAC_DMN_Process *mac_dmn_process_from_handle(DMN_Handle handle) { return (MAC_DMN_Process *)mac_dmn_entity_from_handle(handle, MAC_DMN_EntityKind_Process); }
internal MAC_DMN_Thread  *mac_dmn_thread_from_handle(DMN_Handle handle)  { return (MAC_DMN_Thread *)mac_dmn_entity_from_handle(handle, MAC_DMN_EntityKind_Thread); }
internal MAC_DMN_Module  *mac_dmn_module_from_handle(DMN_Handle handle)  { return (MAC_DMN_Module *)mac_dmn_entity_from_handle(handle, MAC_DMN_EntityKind_Module); }

internal MAC_DMN_Process *
mac_dmn_process_from_pid(pid_t pid)
{
  MAC_DMN_Process *result = 0;
  U64 key = (U64)pid;
  U64 hash = u64_hash_from_str8(str8_struct(&key));
  U64 slot_idx = hash%mac_dmn_state->process_from_pid_slots_count;
  for(MAC_DMN_Process *p = mac_dmn_state->process_from_pid_slots[slot_idx].first; p != 0; p = p->pid_next)
  {
    if(p->pid == pid)
    {
      result = p;
      break;
    }
  }
  return result;
}

internal MAC_DMN_Process *
mac_dmn_process_from_task(mach_port_t task)
{
  MAC_DMN_Process *result = 0;
  for EachNode(p, MAC_DMN_Process, mac_dmn_state->first_process)
  {
    if(p->task == task)
    {
      result = p;
      break;
    }
  }
  return result;
}

internal MAC_DMN_Thread *
mac_dmn_thread_from_port(mach_port_t port)
{
  MAC_DMN_Thread *result = 0;
  U64 key = (U64)port;
  U64 hash = u64_hash_from_str8(str8_struct(&key));
  U64 slot_idx = hash%mac_dmn_state->thread_from_port_slots_count;
  for(MAC_DMN_Thread *t = mac_dmn_state->thread_from_port_slots[slot_idx].first; t != 0; t = t->port_next)
  {
    if(t->port == port)
    {
      result = t;
      break;
    }
  }
  return result;
}

////////////////////////////////
//~ rjf: Thread Helpers

internal void
mac_dmn_thread_refresh_identifier_info(MAC_DMN_Thread *thread)
{
  // NOTE: the TSD base (TPIDRRO_EL0) is still 0 when a launched process is
  // stopped at dyld's first instruction, so this is re-queried lazily until
  // it becomes known.
  thread_identifier_info_data_t id_info = {0};
  mach_msg_type_number_t count = THREAD_IDENTIFIER_INFO_COUNT;
  if(thread_info(thread->port, THREAD_IDENTIFIER_INFO, (thread_info_t)&id_info, &count) == KERN_SUCCESS)
  {
    if(id_info.thread_id != 0)
    {
      thread->tid = id_info.thread_id;
    }
    thread->tsd_base_vaddr = id_info.thread_handle;
  }
}

internal U64
mac_dmn_ip_from_thread(MAC_DMN_Thread *thread)
{
  ARCH_Info *arch_info = arch_info_from_arch(thread->process->ctx->arch);
  return arch_ip_from_reg_block(arch_info, thread->reg_block);
}

internal void
mac_dmn_thread_write_ip(MAC_DMN_Thread *thread, U64 ip)
{
  ARCH_Info *arch_info = arch_info_from_arch(thread->process->ctx->arch);
  arch_reg_block_write_ip(arch_info, thread->reg_block, ip);
  thread->is_reg_block_dirty = 1;
}

internal B32
mac_dmn_thread_read_reg_block(MAC_DMN_Thread *thread)
{
  B32 good = 0;
  if(thread->process->ctx->arch == Arch_arm64)
  {
    ARM64_RegBlock *dst = (ARM64_RegBlock *)thread->reg_block;
    
    // rjf: general purpose registers
    {
      arm_thread_state64_t ts = {0};
      mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
      if(thread_get_state(thread->port, ARM_THREAD_STATE64, (thread_state_t)&ts, &count) == KERN_SUCCESS)
      {
        U64 *x = &dst->x0;
        for EachIndex(idx, 29)
        {
          x[idx] = ts.__x[idx];
        }
        dst->fp   = ts.__fp;
        dst->lr   = ts.__lr;
        dst->sp   = ts.__sp;
        dst->pc   = ts.__pc;
        dst->cpsr = ts.__cpsr;
        good = 1;
      }
    }
    
    // rjf: vector/fp registers
    if(good)
    {
      arm_neon_state64_t ns = {0};
      mach_msg_type_number_t count = ARM_NEON_STATE64_COUNT;
      if(thread_get_state(thread->port, ARM_NEON_STATE64, (thread_state_t)&ns, &count) == KERN_SUCCESS)
      {
        MemoryCopy(&dst->v0, ns.__v, sizeof(U128)*32);
        dst->fpsr = ns.__fpsr;
        dst->fpcr = ns.__fpcr;
      }
    }
    
    // rjf: thread pointer registers
    if(thread->tsd_base_vaddr == 0)
    {
      mac_dmn_thread_refresh_identifier_info(thread);
    }
    dst->tpidrro_el0 = thread->tsd_base_vaddr;
    dst->tpidr_el0   = 0;
  }
  thread->regs_are_valid = good;
  return good;
}

internal B32
mac_dmn_thread_write_reg_block(MAC_DMN_Thread *thread)
{
  B32 good = 0;
  if(thread->process->ctx->arch == Arch_arm64)
  {
    ARM64_RegBlock *src = (ARM64_RegBlock *)thread->reg_block;
    
    // rjf: general purpose registers (read first, to preserve any kernel-owned fields)
    {
      arm_thread_state64_t ts = {0};
      mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
      if(thread_get_state(thread->port, ARM_THREAD_STATE64, (thread_state_t)&ts, &count) == KERN_SUCCESS)
      {
        U64 *x = &src->x0;
        for EachIndex(idx, 29)
        {
          ts.__x[idx] = x[idx];
        }
        ts.__fp   = src->fp;
        ts.__lr   = src->lr;
        ts.__sp   = src->sp;
        ts.__pc   = src->pc;
        ts.__cpsr = src->cpsr;
        if(thread_set_state(thread->port, ARM_THREAD_STATE64, (thread_state_t)&ts, ARM_THREAD_STATE64_COUNT) == KERN_SUCCESS)
        {
          good = 1;
        }
      }
    }
    
    // rjf: vector/fp registers
    if(good)
    {
      arm_neon_state64_t ns = {0};
      MemoryCopy(ns.__v, &src->v0, sizeof(U128)*32);
      ns.__fpsr = src->fpsr;
      ns.__fpcr = src->fpcr;
      if(thread_set_state(thread->port, ARM_NEON_STATE64, (thread_state_t)&ns, ARM_NEON_STATE64_COUNT) != KERN_SUCCESS)
      {
        log_infof("mach error: thread_set_state(ARM_NEON_STATE64) failed.\n");
      }
    }
    
    // rjf: writing the thread state clears PSTATE.SS; re-apply the single
    // step request via the debug state, if it is active for this thread.
    if(good && thread->is_single_stepping)
    {
      mac_dmn_thread_set_single_step(thread, 1);
    }
  }
  return good;
}

internal B32
mac_dmn_thread_set_single_step(MAC_DMN_Thread *thread, B32 is_on)
{
  B32 result = 0;
  arm_debug_state64_t ds = {0};
  mach_msg_type_number_t count = ARM_DEBUG_STATE64_COUNT;
  if(thread_get_state(thread->port, ARM_DEBUG_STATE64, (thread_state_t)&ds, &count) == KERN_SUCCESS)
  {
    if(is_on) { ds.__mdscr_el1 |=  ARM64_MDSCR_EL1_SS; }
    else      { ds.__mdscr_el1 &= ~(U64)ARM64_MDSCR_EL1_SS; }
    // NOTE: the kernel sets/clears PSTATE.SS in the thread's saved state to
    // match MDSCR_EL1.SS when this debug state is written.
    if(thread_set_state(thread->port, ARM_DEBUG_STATE64, (thread_state_t)&ds, ARM_DEBUG_STATE64_COUNT) == KERN_SUCCESS)
    {
      result = 1;
    }
    else
    {
      log_infof("mach error: thread_set_state(ARM_DEBUG_STATE64) failed when %s single stepping.\n", is_on ? "enabling" : "disabling");
    }
  }
  thread->is_single_stepping = (is_on && result);
  return result;
}

internal void
mac_dmn_thread_reply_pending_exception(MAC_DMN_Thread *thread, B32 ignore_exception)
{
  if(!thread->has_pending_exception)
  {
    return;
  }
  MAC_DMN_ExceptionRaiseRequest *req = &thread->pending_exception;
  kern_return_t ret_code = KERN_SUCCESS;
  switch(thread->stop_kind)
  {
    default:{}break;
    
    // rjf: soft signals: choose which signal (if any) is delivered to the
    // thread when it resumes
    case MAC_DMN_StopKind_SoftSignal:
    {
      int signo = ignore_exception ? 0 : (int)thread->stop_signo;
      if(ptrace(PT_THUPDATE, thread->process->pid, (caddr_t)(uintptr_t)thread->port, signo) != 0)
      {
        log_infof("ptrace error: PT_THUPDATE failed (errno %d).\n", errno);
      }
    }break;
    
    // rjf: hardware exceptions: KERN_SUCCESS re-executes the faulting
    // instruction; KERN_FAILURE lets the kernel escalate to a BSD signal,
    // which is then reported (and may be delivered) as a soft signal.
    case MAC_DMN_StopKind_HardException:
    {
      ret_code = ignore_exception ? KERN_SUCCESS : KERN_FAILURE;
      if(!ignore_exception)
      {
        thread->escalated_exception = 1;
        thread->escalated_ip = mac_dmn_ip_from_thread(thread);
      }
    }break;
  }
  MAC_DMN_ExceptionRaiseReply reply = {0};
  reply.head.msgh_bits        = MACH_MSGH_BITS(MACH_MSGH_BITS_REMOTE(req->head.msgh_bits), 0);
  reply.head.msgh_size        = sizeof(reply);
  reply.head.msgh_remote_port = req->head.msgh_remote_port;
  reply.head.msgh_local_port  = MACH_PORT_NULL;
  reply.head.msgh_id          = req->head.msgh_id + 100;
  reply.ndr                   = NDR_record;
  reply.ret_code              = ret_code;
  kern_return_t kr = mach_msg(&reply.head, MACH_SEND_MSG, reply.head.msgh_size, 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
  if(kr != MACH_MSG_SUCCESS)
  {
    log_infof("mach error: failed to send exception reply (%s).\n", mach_error_string(kr));
  }
  thread->has_pending_exception = 0;
  thread->stop_kind = MAC_DMN_StopKind_Suspended;
}

internal void
mac_dmn_process_sync_threads(Arena *arena, DMN_EventList *events, MAC_DMN_Process *process, B32 report)
{
  if(process->ctx == 0 || process->task == MACH_PORT_NULL)
  {
    return;
  }
  thread_act_array_t ports = 0;
  mach_msg_type_number_t count = 0;
  if(task_threads(process->task, &ports, &count) != KERN_SUCCESS)
  {
    return;
  }
  
  // rjf: mark all known threads as unseen
  for EachNode(t, MAC_DMN_Thread, process->first_thread)
  {
    t->is_seen = 0;
  }
  
  // rjf: visit threads reported by the kernel
  for EachIndex(idx, count)
  {
    mach_port_t port = ports[idx];
    MAC_DMN_Thread *thread = mac_dmn_thread_from_port(port);
    if(thread != 0 && thread->process == process)
    {
      thread->is_seen = 1;
      // rjf: we already hold a send right under this name; drop the extra one
      mach_port_deallocate(mach_task_self(), port);
    }
    else
    {
      thread = mac_dmn_thread_alloc(process, port);
      thread->is_seen = 1;
      mac_dmn_thread_read_reg_block(thread);
    }
  }
  if(ports != 0)
  {
    vm_deallocate(mach_task_self(), (vm_address_t)ports, (vm_size_t)(count * sizeof(thread_act_t)));
  }
  
  // rjf: report every thread the engine has not been told about yet (including
  // ones first seen through an exception message)
  if(report)
  {
    for EachNode(t, MAC_DMN_Thread, process->first_thread)
    {
      if(!t->is_reported)
      {
        t->is_reported = 1;
        DMN_Event *e = dmn_event_list_push(arena, events);
        e->kind           = DMN_EventKind_CreateThread;
        e->process        = mac_dmn_handle_from_process(process);
        e->thread         = mac_dmn_handle_from_thread(t);
        e->arch           = process->ctx->arch;
        e->code           = (U32)t->tid;
        e->tls_root_vaddr = t->tsd_base_vaddr;
      }
    }
  }
  
  // rjf: threads not reported anymore have exited
  for(MAC_DMN_Thread *t = process->first_thread, *next = 0; t != 0; t = next)
  {
    next = t->next;
    if(!t->is_seen)
    {
      if(report)
      {
        DMN_Event *e = dmn_event_list_push(arena, events);
        e->kind    = DMN_EventKind_ExitThread;
        e->process = mac_dmn_handle_from_process(process);
        e->thread  = mac_dmn_handle_from_thread(t);
        e->code    = 0;
      }
      mac_dmn_thread_release(t);
    }
  }
}

internal void
mac_dmn_process_suspend(MAC_DMN_Process *process)
{
  if(process->task != MACH_PORT_NULL && !process->has_exited)
  {
    kern_return_t kr = task_suspend(process->task);
    if(kr == KERN_SUCCESS)
    {
      process->task_suspend_count += 1;
    }
  }
}

internal void
mac_dmn_process_resume(MAC_DMN_Process *process)
{
  if(process->task == MACH_PORT_NULL || process->has_exited)
  {
    return;
  }
  for(;process->task_suspend_count > 0;)
  {
    kern_return_t kr = task_resume(process->task);
    if(kr != KERN_SUCCESS)
    {
      break;
    }
    process->task_suspend_count -= 1;
  }
}

//- rjf: exception message reception; returns 1 if a message was received and
// recorded on its thread (the owning process is suspended on first receipt)

internal B32
mac_dmn_process_receive_exceptions(MAC_DMN_Process *process_filter, U64 timeout_ms)
{
  B32 got_any = 0;
  for(;;)
  {
    MAC_DMN_ExceptionMsgBuffer buf = {0};
    mach_msg_option_t options = MACH_RCV_MSG|MACH_RCV_INTERRUPT|MACH_RCV_TIMEOUT;
    kern_return_t kr = mach_msg(&buf.head, options, 0, sizeof(buf), mac_dmn_state->port_set, (mach_msg_timeout_t)timeout_ms, MACH_PORT_NULL);
    if(kr != MACH_MSG_SUCCESS)
    {
      break;
    }
    
    // rjf: unknown message id -> drop
    if(buf.head.msgh_id != MAC_DMN_MSG_ID_EXCEPTION_RAISE)
    {
      log_infof("mach: dropping unexpected message id %d on exception port.\n", (int)buf.head.msgh_id);
      mach_msg_destroy(&buf.head);
      continue;
    }
    
    // rjf: find process
    MAC_DMN_ExceptionRaiseRequest *req = &buf.request;
    MAC_DMN_Process *process = mac_dmn_process_from_task(req->task.name);
    if(process == 0 || process->has_exited)
    {
      // rjf: not ours (or already gone) -> reply success so the thread continues
      MAC_DMN_ExceptionRaiseReply reply = {0};
      reply.head.msgh_bits        = MACH_MSGH_BITS(MACH_MSGH_BITS_REMOTE(req->head.msgh_bits), 0);
      reply.head.msgh_size        = sizeof(reply);
      reply.head.msgh_remote_port = req->head.msgh_remote_port;
      reply.head.msgh_local_port  = MACH_PORT_NULL;
      reply.head.msgh_id          = req->head.msgh_id + 100;
      reply.ndr                   = NDR_record;
      reply.ret_code              = KERN_SUCCESS;
      mach_msg(&reply.head, MACH_SEND_MSG, reply.head.msgh_size, 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
      mach_port_deallocate(mach_task_self(), req->thread.name);
      mach_port_deallocate(mach_task_self(), req->task.name);
      continue;
    }
    
    // rjf: stop the whole task on the first exception of a round
    if(process->task_suspend_count == 0)
    {
      mac_dmn_process_suspend(process);
    }
    
    // rjf: find thread (sync first, in case it is new)
    MAC_DMN_Thread *thread = mac_dmn_thread_from_port(req->thread.name);
    if(thread == 0 && process->ctx != 0)
    {
      mac_dmn_process_sync_threads(0, 0, process, 0);
      thread = mac_dmn_thread_from_port(req->thread.name);
    }
    if(thread == 0 && process->ctx == 0)
    {
      // rjf: process context not yet created (initial attach stop) -> create a bare thread record
      MAC_DMN_ProcessCtx *ctx = &mac_dmn_entity_alloc(MAC_DMN_EntityKind_ProcessCtx)->process_ctx;
      ctx->arena = arena_alloc();
      ctx->arch = Arch_arm64;
      ctx->module_slots_count = 4096;
      ctx->module_slots = push_array(ctx->arena, MAC_DMN_ModuleSlot, ctx->module_slots_count);
      ctx->ref_count = 1;
      process->ctx = ctx;
      mac_dmn_process_sync_threads(0, 0, process, 0);
      thread = mac_dmn_thread_from_port(req->thread.name);
    }
    
    // rjf: drop the extra send rights carried by the message
    mach_port_deallocate(mach_task_self(), req->thread.name);
    mach_port_deallocate(mach_task_self(), req->task.name);
    
    if(thread == 0)
    {
      log_infof("mach: exception from unknown thread port %u.\n", (unsigned)req->thread.name);
      continue;
    }
    
    // rjf: record on thread
    if(thread->has_pending_exception && thread->pending_exception.head.msgh_remote_port != MACH_PORT_NULL)
    {
      // rjf: should not happen (a thread cannot raise twice without a reply); do not leak the old reply right
      mach_port_deallocate(mach_task_self(), thread->pending_exception.head.msgh_remote_port);
    }
    thread->has_pending_exception = 1;
    MemoryCopyStruct(&thread->pending_exception, req);
    thread->state = MAC_DMN_ThreadState_Stopped;
    thread->is_reg_block_dirty = 0;
    mac_dmn_thread_read_reg_block(thread);
    thread->stop_signo = 0;
    thread->stop_hw_trap_vaddr = 0;
    thread->force_ignore_on_reply = 0;
    {
      exception_type_t exception = req->exception;
      S64 code0 = (req->code_count > 0) ? req->code[0] : 0;
      S64 code1 = (req->code_count > 1) ? req->code[1] : 0;
      switch(exception)
      {
        default:
        {
          thread->stop_kind = MAC_DMN_StopKind_HardException;
        }break;
        case EXC_SOFTWARE:
        {
          if(code0 == EXC_SOFT_SIGNAL)
          {
            thread->stop_kind = MAC_DMN_StopKind_SoftSignal;
            thread->stop_signo = (U64)code1;
          }
          else
          {
            thread->stop_kind = MAC_DMN_StopKind_HardException;
          }
        }break;
        case EXC_BREAKPOINT:
        {
          // rjf: classify via the exception syndrome when available: software
          // step (EC 0x32), brk (EC 0x3c), hardware breakpoint (EC 0x30),
          // watchpoint (EC 0x34)
          ARM64_ESR_EC ec = ARM64_ESR_EC_Unknown;
          {
            arm_exception_state64_t es = {0};
            mach_msg_type_number_t count = ARM_EXCEPTION_STATE64_COUNT;
            if(thread_get_state(thread->port, ARM_EXCEPTION_STATE64, (thread_state_t)&es, &count) == KERN_SUCCESS)
            {
              ec = ARM64_ESR_EC_FROM_ESR(es.__esr);
            }
          }
          U64 ip = mac_dmn_ip_from_thread(thread);
          U32 inst = 0;
          B32 at_brk = (mac_dmn_read_struct(process->task, ip, &inst) == sizeof(inst) && arm64_inst_is_brk(inst));
          if(code0 == EXC_ARM_DA_DEBUG || ec == ARM64_ESR_EC_WatchpointLower || ec == ARM64_ESR_EC_WatchpointCurrent)
          {
            thread->stop_kind = MAC_DMN_StopKind_Watchpoint;
            thread->stop_hw_trap_vaddr = (U64)code1;
          }
          else if(ec == ARM64_ESR_EC_SoftwareStepLower || ec == ARM64_ESR_EC_SoftwareStepCurrent)
          {
            thread->stop_kind = MAC_DMN_StopKind_SingleStep;
          }
          else if(ec == ARM64_ESR_EC_BRK || at_brk)
          {
            thread->stop_kind = MAC_DMN_StopKind_Breakpoint;
          }
          else if(thread->is_single_stepping)
          {
            thread->stop_kind = MAC_DMN_StopKind_SingleStep;
          }
          else
          {
            // rjf: a hardware breakpoint we did not set, or a stale trap report ->
            // nothing to report; continue silently
            thread->stop_kind = MAC_DMN_StopKind_Suspended;
            thread->force_ignore_on_reply = 1;
          }
        }break;
      }
    }
    got_any = 1;
    
    // rjf: after the first message, only poll briefly for stragglers
    timeout_ms = Min(timeout_ms, 2);
  }
  return got_any;
}

////////////////////////////////
//~ rjf: Module / dyld Helpers

internal DMN_ModuleInfo *
mac_dmn_module_info_from_image(Arena *arena, MAC_DMN_Process *process, MAC_DMN_ImageInfo *image, String8 path)
{
  DMN_ModuleInfo *info = push_array(arena, DMN_ModuleInfo, 1);
  Arch arch = Arch_Null;
  switch(image->cputype)
  {
    default:{}break;
    case CPU_TYPE_ARM64:  {arch = Arch_arm64;}break;
    case CPU_TYPE_X86_64: {arch = Arch_x64;}break;
  }
  info->arch                        = arch;
  info->vsize                       = image->vsize;
  info->entry_point_voff            = image->entry_point_voff;
  info->module_path                 = push_str8_copy(arena, path);
  info->debug_info_path             = info->module_path;
  info->debug_info_guid             = image->uuid;
  info->debug_info_timestamp        = 0;
  info->debug_info_age              = 0;
  info->tls_index                   = max_U64;
  info->tls_offset                  = max_U64;
  info->macho_unwind_info_vaddr_range = image->unwind_info_vaddr_range;
  info->macho_eh_frame_vaddr_range    = image->eh_frame_vaddr_range;
  info->macho_text_vaddr_range        = image->text_vaddr_range;
  return info;
}

internal void
mac_dmn_push_event_load_module(Arena *arena, DMN_EventList *events, MAC_DMN_Process *process, MAC_DMN_Thread *thread, U64 header_vaddr, String8 path, B32 is_main, B32 is_dyld)
{
  MAC_DMN_ProcessCtx *ctx = process->ctx;
  if(mac_dmn_module_from_base_vaddr(ctx, header_vaddr) != 0)
  {
    return;
  }
  MAC_DMN_ImageInfo image = mac_dmn_image_info_from_header_vaddr(process->task, header_vaddr);
  if(!image.valid)
  {
    return;
  }
  MAC_DMN_Module *module = mac_dmn_module_alloc(ctx, header_vaddr, image.vsize, path, image.thread_vars_vaddr, is_main, is_dyld);
  DMN_ModuleInfo *module_info = mac_dmn_module_info_from_image(arena, process, &image, module->path);
  DMN_Event *e = dmn_event_list_push(arena, events);
  e->kind        = DMN_EventKind_LoadModule;
  e->process     = mac_dmn_handle_from_process(process);
  e->thread      = mac_dmn_handle_from_thread(thread);
  e->module      = mac_dmn_handle_from_module(module);
  e->arch        = module_info->arch;
  e->address     = module->base_vaddr;
  e->size        = module->size;
  e->string      = module_info->module_path;
  e->module_info = module_info;
}

internal void
mac_dmn_push_event_unload_module(Arena *arena, DMN_EventList *events, MAC_DMN_Process *process, MAC_DMN_Module *module)
{
  DMN_Event *e = dmn_event_list_push(arena, events);
  e->kind    = DMN_EventKind_UnloadModule;
  e->process = mac_dmn_handle_from_process(process);
  e->module  = mac_dmn_handle_from_module(module);
}

//- rjf: diff the dyld image list against our module list (used after attach
// and as a safety net after notifications)

internal void
mac_dmn_process_sync_images(Arena *arena, DMN_EventList *events, MAC_DMN_Process *process, MAC_DMN_Thread *thread)
{
  if(process->all_image_infos_vaddr == 0 || process->ctx == 0)
  {
    return;
  }
  Temp scratch = scratch_begin(&arena, 1);
  struct dyld_all_image_infos infos = {0};
  U64 read_size = mac_dmn_read(process->task, r1u64(process->all_image_infos_vaddr, process->all_image_infos_vaddr + sizeof(infos)), &infos);
  if(read_size >= OffsetOf(struct dyld_all_image_infos, notification) + sizeof(infos.notification) && infos.infoArrayCount < 65536 && infos.infoArray != 0)
  {
    // rjf: read image array
    U64 count = infos.infoArrayCount;
    struct dyld_image_info *array = push_array_no_zero(scratch.arena, struct dyld_image_info, count);
    U64 array_vaddr = (U64)(uintptr_t)infos.infoArray;
    U64 array_read = mac_dmn_read(process->task, r1u64(array_vaddr, array_vaddr + count*sizeof(array[0])), array);
    count = array_read / sizeof(array[0]);
    
    // rjf: mark all modules dead except dyld & the main executable
    for(MAC_DMN_Module *m = process->ctx->first_module; m != 0; m = m->order_next)
    {
      m->is_live = (m->is_dyld || m->is_main);
    }
    
    // rjf: load new images, mark present ones live
    for EachIndex(idx, count)
    {
      U64 header_vaddr = (U64)(uintptr_t)array[idx].imageLoadAddress;
      MAC_DMN_Module *existing = mac_dmn_module_from_base_vaddr(process->ctx, header_vaddr);
      if(existing != 0)
      {
        existing->is_live = 1;
      }
      else
      {
        String8 path = mac_dmn_read_string_capped(scratch.arena, process->task, (U64)(uintptr_t)array[idx].imageFilePath, 4096);
        struct mach_header_64 hdr = {0};
        B32 is_main = (mac_dmn_read_struct(process->task, header_vaddr, &hdr) == sizeof(hdr) && hdr.filetype == MH_EXECUTE);
        mac_dmn_push_event_load_module(arena, events, process, thread, header_vaddr, path, is_main, 0);
      }
    }
    
    // rjf: unload dead modules
    for(MAC_DMN_Module *m = process->ctx->first_module, *next = 0; m != 0; m = next)
    {
      next = m->order_next;
      if(!m->is_live)
      {
        mac_dmn_push_event_unload_module(arena, events, process, m);
        mac_dmn_module_release(process->ctx, m);
      }
    }
    process->last_image_info_count = infos.infoArrayCount;
  }
  scratch_end(scratch);
}

internal void
mac_dmn_process_set_dyld_notification_trap(MAC_DMN_Process *process, B32 is_set)
{
  if(process->dyld_notification_vaddr == 0 || process->has_exited)
  {
    return;
  }
  U64 vaddr = process->dyld_notification_vaddr;
  if(is_set && !process->dyld_notification_trap_is_set)
  {
    U32 orig = 0;
    if(mac_dmn_read_struct(process->task, vaddr, &orig) == sizeof(orig))
    {
      U32 brk = ARM64_INST_BRK0;
      if(mac_dmn_write_struct(process->task, vaddr, &brk))
      {
        process->dyld_notification_orig_inst = orig;
        process->dyld_notification_trap_is_set = 1;
      }
      else
      {
        log_infof("mach: failed to write dyld notification breakpoint at 0x%I64x.\n", vaddr);
      }
    }
  }
  else if(!is_set && process->dyld_notification_trap_is_set)
  {
    mac_dmn_write_struct(process->task, vaddr, &process->dyld_notification_orig_inst);
    process->dyld_notification_trap_is_set = 0;
  }
}

internal U64
mac_dmn_strip_pac(U64 ptr)
{
  // NOTE: user-space pointers use at most 47 bits on arm64 macOS; pointer
  // authentication signature bits (arm64e system binaries such as dyld) live above.
  return ptr & ((1ull << 47) - 1);
}

internal B32
mac_dmn_process_find_dyld_notification(MAC_DMN_Process *process, U64 dyld_header_vaddr, B32 dyld_is_rebased)
{
  process->dyld_base_vaddr = dyld_header_vaddr;
  process->dyld_notification_vaddr = 0;
  MAC_DMN_ImageInfo dyld_image = mac_dmn_image_info_from_header_vaddr(process->task, dyld_header_vaddr);
  if(dyld_image.valid)
  {
    U64 vaddr = mac_dmn_symbol_vaddr_from_image(process->task, &dyld_image, str8_lit("__dyld_debugger_notification"));
    if(vaddr == 0)
    {
      vaddr = mac_dmn_symbol_vaddr_from_image(process->task, &dyld_image, str8_lit("_dyld_debugger_notification"));
    }
    // rjf: fall back to the function pointer published in dyld_all_image_infos. it is
    // only meaningful once dyld has rebased itself (it is a chained fixup before),
    // and it carries pointer-authentication bits on arm64e dylds.
    if(vaddr == 0 && dyld_is_rebased && process->all_image_infos_vaddr != 0)
    {
      struct dyld_all_image_infos infos = {0};
      U64 read_size = mac_dmn_read(process->task, r1u64(process->all_image_infos_vaddr, process->all_image_infos_vaddr + sizeof(infos)), &infos);
      if(read_size >= OffsetOf(struct dyld_all_image_infos, notification) + sizeof(infos.notification) && infos.notification != 0)
      {
        vaddr = mac_dmn_strip_pac((U64)(uintptr_t)infos.notification);
      }
    }
    process->dyld_notification_vaddr = vaddr;
  }
  if(process->dyld_notification_vaddr == 0)
  {
    log_infof("mac demon: could not find _dyld_debugger_notification in dyld (pid %d); dynamic library load events will be synthesized at stops.\n", (int)process->pid);
  }
  return (process->dyld_notification_vaddr != 0);
}

//- rjf: per-stop safety net: pick up images that were loaded without a
// notification (trap not placed yet, dyld moved, ...)

internal void
mac_dmn_process_check_images(Arena *arena, DMN_EventList *events, MAC_DMN_Process *process)
{
  if(process->ctx == 0 || process->all_image_infos_vaddr == 0 || process->has_exited || process->state != MAC_DMN_ProcessState_Normal)
  {
    return;
  }
  struct dyld_all_image_infos infos = {0};
  U64 read_size = mac_dmn_read(process->task, r1u64(process->all_image_infos_vaddr, process->all_image_infos_vaddr + sizeof(infos)), &infos);
  if(read_size < OffsetOf(struct dyld_all_image_infos, notification) + sizeof(infos.notification))
  {
    return;
  }
  if(infos.infoArrayCount == 0 || infos.infoArray == 0)
  {
    // rjf: dyld has not initialized yet (first instruction)
    return;
  }
  // rjf: trap not placed yet (e.g. symbol lookup failed before dyld rebased itself)
  if(process->dyld_notification_vaddr == 0 && infos.notification != 0)
  {
    process->dyld_notification_vaddr = mac_dmn_strip_pac((U64)(uintptr_t)infos.notification);
    mac_dmn_process_set_dyld_notification_trap(process, 1);
  }
  if(infos.infoArrayCount != process->last_image_info_count || !process->dyld_notification_trap_is_set)
  {
    mac_dmn_process_sync_images(arena, events, process, process->first_thread);
  }
}

//- rjf: process context initialization; emits process/thread/module creation
// events. the task must be suspended.

internal B32
mac_dmn_process_init_ctx(Arena *arena, DMN_EventList *events, MAC_DMN_Process *process, B32 is_launch)
{
  Temp scratch = scratch_begin(&arena, 1);
  B32 good = 0;
  
  //- rjf: allocate context, if not already
  MAC_DMN_ProcessCtx *ctx = process->ctx;
  if(ctx == 0)
  {
    ctx = &mac_dmn_entity_alloc(MAC_DMN_EntityKind_ProcessCtx)->process_ctx;
    ctx->arena = arena_alloc();
    ctx->arch = Arch_arm64;
    ctx->module_slots_count = 4096;
    ctx->module_slots = push_array(ctx->arena, MAC_DMN_ModuleSlot, ctx->module_slots_count);
    ctx->ref_count = 1;
    process->ctx = ctx;
  }
  
  //- rjf: gather threads
  mac_dmn_process_sync_threads(0, 0, process, 0);
  MAC_DMN_Thread *main_thread = process->first_thread;
  
  //- rjf: gather dyld info
  {
    task_dyld_info_data_t dyld_info = {0};
    mach_msg_type_number_t count = TASK_DYLD_INFO_COUNT;
    if(task_info(process->task, TASK_DYLD_INFO, (task_info_t)&dyld_info, &count) == KERN_SUCCESS)
    {
      process->all_image_infos_vaddr = dyld_info.all_image_info_addr;
    }
  }
  
  //- rjf: locate main executable & dyld
  U64 main_header_vaddr = 0;
  U64 dyld_header_vaddr = 0;
  if(main_thread != 0)
  {
    ARM64_RegBlock *regs = (ARM64_RegBlock *)main_thread->reg_block;
    if(is_launch)
    {
      // rjf: at the first instruction of dyld, the kernel left the address of
      // the main executable's mach header on top of the stack
      U64 mh = 0;
      if(mac_dmn_read_struct(process->task, regs->sp, &mh) == sizeof(mh))
      {
        struct mach_header_64 hdr = {0};
        if(mac_dmn_read_struct(process->task, mh, &hdr) == sizeof(hdr) && hdr.magic == MH_MAGIC_64 && hdr.filetype == MH_EXECUTE)
        {
          main_header_vaddr = mh;
        }
      }
      dyld_header_vaddr = mac_dmn_image_header_vaddr_from_vaddr_scan(process->task, regs->pc, MH_DYLINKER);
    }
    else if(process->all_image_infos_vaddr != 0)
    {
      struct dyld_all_image_infos infos = {0};
      U64 read_size = mac_dmn_read(process->task, r1u64(process->all_image_infos_vaddr, process->all_image_infos_vaddr + sizeof(infos)), &infos);
      if(read_size >= OffsetOf(struct dyld_all_image_infos, dyldImageLoadAddress) + sizeof(infos.dyldImageLoadAddress))
      {
        dyld_header_vaddr = (U64)(uintptr_t)infos.dyldImageLoadAddress;
      }
      if(infos.infoArray != 0 && infos.infoArrayCount != 0 && infos.infoArrayCount < 65536)
      {
        U64 count = infos.infoArrayCount;
        struct dyld_image_info *array = push_array_no_zero(scratch.arena, struct dyld_image_info, count);
        U64 array_vaddr = (U64)(uintptr_t)infos.infoArray;
        U64 array_read = mac_dmn_read(process->task, r1u64(array_vaddr, array_vaddr + count*sizeof(array[0])), array);
        count = array_read / sizeof(array[0]);
        for EachIndex(idx, count)
        {
          U64 header_vaddr = (U64)(uintptr_t)array[idx].imageLoadAddress;
          struct mach_header_64 hdr = {0};
          if(mac_dmn_read_struct(process->task, header_vaddr, &hdr) == sizeof(hdr) && hdr.filetype == MH_EXECUTE)
          {
            main_header_vaddr = header_vaddr;
            break;
          }
        }
      }
      if(main_header_vaddr == 0)
      {
        // rjf: fall back to scanning from the main thread's pc (works when stopped in the main image)
        main_header_vaddr = mac_dmn_image_header_vaddr_from_vaddr_scan(process->task, regs->pc, MH_EXECUTE);
      }
    }
    if(main_header_vaddr == 0)
    {
      // rjf: last resort: walk the address space looking for an MH_EXECUTE header
      mach_vm_address_t addr = 0;
      for(U64 idx = 0; idx < 4096; idx += 1)
      {
        mach_vm_size_t size = 0;
        natural_t depth = 0;
        vm_region_submap_info_data_64_t info = {0};
        mach_msg_type_number_t info_count = VM_REGION_SUBMAP_INFO_COUNT_64;
        if(mach_vm_region_recurse(process->task, &addr, &size, &depth, (vm_region_recurse_info_t)&info, &info_count) != KERN_SUCCESS)
        {
          break;
        }
        struct mach_header_64 hdr = {0};
        if((info.protection & VM_PROT_READ) && mac_dmn_read_struct(process->task, addr, &hdr) == sizeof(hdr) && hdr.magic == MH_MAGIC_64)
        {
          if(hdr.filetype == MH_EXECUTE && main_header_vaddr == 0) { main_header_vaddr = addr; }
          if(hdr.filetype == MH_DYLINKER && dyld_header_vaddr == 0) { dyld_header_vaddr = addr; }
        }
        if(main_header_vaddr != 0 && dyld_header_vaddr != 0) { break; }
        addr += size;
      }
    }
  }
  
  //- rjf: verify architecture
  Arch arch = Arch_Null;
  if(main_header_vaddr != 0)
  {
    struct mach_header_64 hdr = {0};
    if(mac_dmn_read_struct(process->task, main_header_vaddr, &hdr) == sizeof(hdr))
    {
      if(hdr.cputype == CPU_TYPE_ARM64) { arch = Arch_arm64; }
    }
  }
  if(arch == Arch_Null)
  {
    log_infof("mac demon: could not locate a native arm64 main executable image for pid %d (x86_64/Rosetta processes are not supported).\n", (int)process->pid);
    DMN_Event *e = dmn_event_list_push(arena, events);
    e->kind       = DMN_EventKind_Error;
    e->error_kind = DMN_ErrorKind_UnexpectedFailure;
    e->process    = mac_dmn_handle_from_process(process);
    e->string     = str8_lit("The target is not a native arm64 process; x86_64 (Rosetta) processes are not supported.");
    scratch_end(scratch);
    return 0;
  }
  ctx->arch = arch;
  
  //- rjf: main executable path
  String8 main_path = {0};
  {
    char *buffer = push_array(scratch.arena, char, PROC_PIDPATHINFO_MAXSIZE);
    int size = proc_pidpath(process->pid, buffer, PROC_PIDPATHINFO_MAXSIZE);
    if(size > 0)
    {
      main_path = str8((U8 *)buffer, (U64)size);
    }
  }
  
  //- rjf: locate dyld's debugger notification function
  if(dyld_header_vaddr != 0)
  {
    mac_dmn_process_find_dyld_notification(process, dyld_header_vaddr, !is_launch);
  }
  
  //- rjf: report process creation
  {
    DMN_Event *e = dmn_event_list_push(arena, events);
    e->kind    = DMN_EventKind_CreateProcess;
    e->process = mac_dmn_handle_from_process(process);
    e->arch    = ctx->arch;
    e->code    = (U32)process->pid;
  }
  
  //- rjf: report threads
  for EachNode(t, MAC_DMN_Thread, process->first_thread)
  {
    t->is_reported = 1;
    DMN_Event *e = dmn_event_list_push(arena, events);
    e->kind           = DMN_EventKind_CreateThread;
    e->process        = mac_dmn_handle_from_process(process);
    e->thread         = mac_dmn_handle_from_thread(t);
    e->arch           = ctx->arch;
    e->code           = (U32)t->tid;
    e->tls_root_vaddr = t->tsd_base_vaddr;
  }
  
  //- rjf: report modules
  if(main_header_vaddr != 0)
  {
    mac_dmn_push_event_load_module(arena, events, process, main_thread, main_header_vaddr, main_path, 1, 0);
  }
  if(dyld_header_vaddr != 0)
  {
    mac_dmn_push_event_load_module(arena, events, process, main_thread, dyld_header_vaddr, str8_lit("/usr/lib/dyld"), 0, 1);
  }
  if(!is_launch)
  {
    mac_dmn_process_sync_images(arena, events, process, main_thread);
  }
  
  //- rjf: install dyld notification trap
  mac_dmn_process_set_dyld_notification_trap(process, 1);
  
  //- rjf: report handshake completion
  {
    DMN_Event *e = dmn_event_list_push(arena, events);
    e->kind    = DMN_EventKind_HandshakeComplete;
    e->process = mac_dmn_handle_from_process(process);
    e->thread  = mac_dmn_handle_from_thread(main_thread);
  }
  
  //- rjf: process is now in its normal state
  if(process->state != MAC_DMN_ProcessState_Normal)
  {
    process->state = MAC_DMN_ProcessState_Normal;
    mac_dmn_state->process_pending_creation -= 1;
  }
  good = 1;
  scratch_end(scratch);
  return good;
}

////////////////////////////////
//~ rjf: Internal Stepping (stepping over demon-owned traps / watchpoints)

internal void
mac_dmn_thread_step_over_internal(MAC_DMN_Process *process, MAC_DMN_Thread *thread)
{
  if(process->has_exited || thread->state == MAC_DMN_ThreadState_Exited)
  {
    return;
  }
  U64 pc = mac_dmn_ip_from_thread(thread);
  B32 is_dyld_trap  = (process->dyld_notification_trap_is_set && pc == process->dyld_notification_vaddr);
  B32 is_watchpoint = (thread->stop_kind == MAC_DMN_StopKind_Watchpoint);
  if(!is_dyld_trap && !is_watchpoint)
  {
    return;
  }
  
  //- rjf: temporarily remove the trap / watchpoints
  arm_debug_state64_t saved_dbg = {0};
  B32 have_saved_dbg = 0;
  if(is_dyld_trap)
  {
    mac_dmn_process_set_dyld_notification_trap(process, 0);
  }
  if(is_watchpoint)
  {
    mach_msg_type_number_t count = ARM_DEBUG_STATE64_COUNT;
    if(thread_get_state(thread->port, ARM_DEBUG_STATE64, (thread_state_t)&saved_dbg, &count) == KERN_SUCCESS)
    {
      have_saved_dbg = 1;
      arm_debug_state64_t cleared = saved_dbg;
      for EachIndex(idx, 16)
      {
        cleared.__wvr[idx] = 0;
        cleared.__wcr[idx] = 0;
      }
      thread_set_state(thread->port, ARM_DEBUG_STATE64, (thread_state_t)&cleared, ARM_DEBUG_STATE64_COUNT);
    }
  }
  
  //- rjf: suspend all other threads of this process
  for EachNode(t, MAC_DMN_Thread, process->first_thread)
  {
    t->is_seen = 0;
    if(t != thread && !t->is_suspended_by_us && t->state != MAC_DMN_ThreadState_Exited)
    {
      if(thread_suspend(t->port) == KERN_SUCCESS)
      {
        t->is_suspended_by_us = 1;
        t->is_seen = 1; // rjf: mark: suspended for this internal step
      }
    }
  }
  
  //- rjf: single-step the thread
  if(thread->is_reg_block_dirty && thread->regs_are_valid)
  {
    thread->is_reg_block_dirty = !mac_dmn_thread_write_reg_block(thread);
  }
  B32 was_single_stepping = thread->is_single_stepping;
  mac_dmn_thread_set_single_step(thread, 1);
  mac_dmn_thread_reply_pending_exception(thread, 1);
  thread->state = MAC_DMN_ThreadState_Running;
  thread->regs_are_valid = 0;
  mac_dmn_process_resume(process);
  for(U64 attempt = 0; attempt < 50 && !thread->has_pending_exception; attempt += 1)
  {
    mac_dmn_process_receive_exceptions(process, 100);
  }
  if(process->task_suspend_count == 0)
  {
    mac_dmn_process_suspend(process);
  }
  if(!thread->has_pending_exception)
  {
    log_infof("mac demon: internal single step of thread %I64u did not complete.\n", thread->tid);
  }
  thread->state = MAC_DMN_ThreadState_Stopped;
  if(!thread->regs_are_valid)
  {
    mac_dmn_thread_read_reg_block(thread);
  }
  mac_dmn_thread_set_single_step(thread, 0);
  if(thread->stop_kind == MAC_DMN_StopKind_SingleStep)
  {
    // rjf: step completion; its pending reply is a plain continue
    thread->stop_kind = MAC_DMN_StopKind_Suspended;
  }
  
  //- rjf: resume the other threads we suspended for this step
  for EachNode(t, MAC_DMN_Thread, process->first_thread)
  {
    if(t != thread && t->is_seen && t->is_suspended_by_us)
    {
      thread_resume(t->port);
      t->is_suspended_by_us = 0;
    }
    t->is_seen = 0;
  }
  
  //- rjf: restore the trap / watchpoints
  if(is_dyld_trap)
  {
    mac_dmn_process_set_dyld_notification_trap(process, 1);
  }
  if(have_saved_dbg)
  {
    saved_dbg.__mdscr_el1 &= ~(U64)ARM64_MDSCR_EL1_SS;
    thread_set_state(thread->port, ARM_DEBUG_STATE64, (thread_state_t)&saved_dbg, ARM_DEBUG_STATE64_COUNT);
  }
  if(was_single_stepping)
  {
    // rjf: the engine asked for a single step this run; the internal step consumed
    // it, so report the step completion instead of re-running
    thread->stop_kind = MAC_DMN_StopKind_SingleStep;
    thread->stop_reported = 0;
  }
}

////////////////////////////////
//~ rjf: Hardware Trap (Watchpoint) Programming

internal void
mac_dmn_process_set_hw_traps(MAC_DMN_Process *process, DMN_TrapChunkList *traps, B32 enable)
{
  if(process->has_exited)
  {
    return;
  }
  DMN_Handle process_handle = mac_dmn_handle_from_process(process);
  for EachNode(thread, MAC_DMN_Thread, process->first_thread)
  {
    if(thread->state == MAC_DMN_ThreadState_Exited) { continue; }
    arm_debug_state64_t ds = {0};
    mach_msg_type_number_t count = ARM_DEBUG_STATE64_COUNT;
    if(thread_get_state(thread->port, ARM_DEBUG_STATE64, (thread_state_t)&ds, &count) != KERN_SUCCESS)
    {
      continue;
    }
    for EachIndex(idx, 16)
    {
      ds.__wvr[idx] = 0;
      ds.__wcr[idx] = 0;
    }
    if(enable)
    {
      U64 wp_idx = 0;
      for EachNode(n, DMN_TrapChunkNode, traps->first)
      {
        for EachIndex(n_idx, n->count)
        {
          DMN_Trap *trap = n->v+n_idx;
          if(trap->flags == 0 || !dmn_handle_match(trap->process, process_handle)) { continue; }
          if(!(trap->flags & (DMN_TrapFlag_BreakOnRead|DMN_TrapFlag_BreakOnWrite))) { continue; }
          if(wp_idx >= 16) { break; }
          U64 size = trap->size ? trap->size : 8;
          U64 base = trap->vaddr & ~(U64)7;
          U64 lo_byte = trap->vaddr - base;
          U64 byte_count = Min(size, 8 - lo_byte);
          U64 bas = ((1ull << byte_count) - 1) << lo_byte;
          U64 lsc = 0;
          if(trap->flags & DMN_TrapFlag_BreakOnRead)  { lsc |= ARM64_DBGWCR_LSC_LOAD; }
          if(trap->flags & DMN_TrapFlag_BreakOnWrite) { lsc |= ARM64_DBGWCR_LSC_STORE; }
          ds.__wvr[wp_idx] = base;
          ds.__wcr[wp_idx] = ARM64_DBGWCR_E | ARM64_DBGWCR_PAC_EL0 | lsc | (bas << ARM64_DBGWCR_BAS_SHIFT);
          wp_idx += 1;
        }
      }
    }
    thread_set_state(thread->port, ARM_DEBUG_STATE64, (thread_state_t)&ds, ARM_DEBUG_STATE64_COUNT);
  }
}

////////////////////////////////
//~ rjf: Exception Port Setup

internal B32
mac_dmn_process_install_exception_ports(MAC_DMN_Process *process)
{
  exception_mask_t mask = (EXC_MASK_BAD_ACCESS|EXC_MASK_BAD_INSTRUCTION|EXC_MASK_ARITHMETIC|EXC_MASK_EMULATION|
                           EXC_MASK_SOFTWARE|EXC_MASK_BREAKPOINT|EXC_MASK_SYSCALL|EXC_MASK_MACH_SYSCALL|
                           EXC_MASK_RPC_ALERT|EXC_MASK_GUARD);
  process->saved_exc_count = EXC_TYPES_COUNT;
  kern_return_t get_kr = task_get_exception_ports(process->task, mask, process->saved_exc_masks, &process->saved_exc_count, process->saved_exc_ports, process->saved_exc_behaviors, process->saved_exc_flavors);
  if(get_kr != KERN_SUCCESS)
  {
    process->saved_exc_count = 0;
  }
  kern_return_t set_kr = task_set_exception_ports(process->task, mask, mac_dmn_state->exception_port, (exception_behavior_t)(EXCEPTION_DEFAULT|MACH_EXCEPTION_CODES), THREAD_STATE_NONE);
  if(set_kr != KERN_SUCCESS)
  {
    log_infof("mach error: task_set_exception_ports failed (%s).\n", mach_error_string(set_kr));
  }
  return (set_kr == KERN_SUCCESS);
}

internal void
mac_dmn_process_restore_exception_ports(MAC_DMN_Process *process)
{
  for EachIndex(idx, process->saved_exc_count)
  {
    task_set_exception_ports(process->task, process->saved_exc_masks[idx], process->saved_exc_ports[idx], process->saved_exc_behaviors[idx], process->saved_exc_flavors[idx]);
    if(process->saved_exc_ports[idx] != MACH_PORT_NULL)
    {
      mach_port_deallocate(mach_task_self(), process->saved_exc_ports[idx]);
    }
  }
  process->saved_exc_count = 0;
}

////////////////////////////////
//~ rjf: @dmn_os_hooks Main Layer Initialization (Implemented Per-OS)

internal void
dmn_init(void)
{
  Arena *arena = arena_alloc();
  mac_dmn_state = push_array(arena, MAC_DMN_State, 1);
  mac_dmn_state->arena          = arena;
  mac_dmn_state->access_mutex   = mutex_alloc();
  mac_dmn_state->entities_arena = arena_alloc(.reserve_size = GB(32), .commit_size = KB(64), .flags = ArenaFlag_NoChain);
  mac_dmn_state->entities_base  = push_array(mac_dmn_state->entities_arena, MAC_DMN_Entity, 0);
  mac_dmn_state->halter_mutex   = mutex_alloc();
  mac_dmn_state->page_size      = get_system_info()->page_size;
  if(mac_dmn_state->page_size == 0)
  {
    mac_dmn_state->page_size = KB(16);
  }
  mac_dmn_entity_alloc(MAC_DMN_EntityKind_Null);
  
  // rjf: set up id <-> entity tables
  {
    mac_dmn_state->process_from_pid_slots_count = 4096;
    mac_dmn_state->process_from_pid_slots = push_array(arena, MAC_DMN_ProcessSlot, mac_dmn_state->process_from_pid_slots_count);
    mac_dmn_state->thread_from_port_slots_count = 16384;
    mac_dmn_state->thread_from_port_slots = push_array(arena, MAC_DMN_ThreadSlot, mac_dmn_state->thread_from_port_slots_count);
  }
  
  // rjf: set up exception port & port set
  {
    mach_port_t self = mach_task_self();
    kern_return_t kr = mach_port_allocate(self, MACH_PORT_RIGHT_RECEIVE, &mac_dmn_state->exception_port);
    if(kr == KERN_SUCCESS)
    {
      kr = mach_port_insert_right(self, mac_dmn_state->exception_port, mac_dmn_state->exception_port, MACH_MSG_TYPE_MAKE_SEND);
    }
    if(kr == KERN_SUCCESS)
    {
      kr = mach_port_allocate(self, MACH_PORT_RIGHT_PORT_SET, &mac_dmn_state->port_set);
    }
    if(kr == KERN_SUCCESS)
    {
      kr = mach_port_move_member(self, mac_dmn_state->exception_port, mac_dmn_state->port_set);
    }
    if(kr != KERN_SUCCESS)
    {
      log_infof("mach error: failed to set up exception port (%s).\n", mach_error_string(kr));
    }
  }
  
  // rjf: set up kqueue (exception messages, process exits, halt wakeups)
  {
    mac_dmn_state->kq = kqueue();
    struct kevent kevs[2] = {0};
    EV_SET(&kevs[0], (uintptr_t)mac_dmn_state->port_set, EVFILT_MACHPORT, EV_ADD|EV_ENABLE, 0, 0, 0);
    EV_SET(&kevs[1], 1, EVFILT_USER, EV_ADD|EV_CLEAR, 0, 0, 0);
    if(kevent(mac_dmn_state->kq, kevs, ArrayCount(kevs), 0, 0, 0) < 0)
    {
      log_infof("kqueue error: failed to register event filters (errno %d).\n", errno);
    }
  }
}

////////////////////////////////
//~ rjf: @dmn_os_hooks Blocking Control Thread Operations (Implemented Per-OS)

internal DMN_CtrlCtx *
dmn_ctrl_begin(void)
{
  DMN_CtrlCtx *ctx = (DMN_CtrlCtx *)1;
  mac_dmn_ctrl_thread = 1;
  return ctx;
}

internal void
dmn_ctrl_exclusive_access_begin(void)
{
  MutexScope(mac_dmn_state->access_mutex)
  {
    mac_dmn_state->access_run_state = 1;
  }
}

internal void
dmn_ctrl_exclusive_access_end(void)
{
  MutexScope(mac_dmn_state->access_mutex)
  {
    mac_dmn_state->access_run_state = 0;
  }
}

internal U32
dmn_ctrl_launch(DMN_CtrlCtx *ctx, ProcessLaunchParams *params)
{
  Temp scratch = scratch_begin(0, 0);
  pid_t pid = 0;
  
  //- rjf: setup target command line
  U64 argc = params->cmd_line.node_count + 1;
  char **argv = push_array(scratch.arena, char *, argc);
  {
    U64 idx = 0;
    for EachNode(n, String8Node, params->cmd_line.first)
    {
      argv[idx] = (char *)push_str8_copy(scratch.arena, n->string).str;
      idx += 1;
    }
  }
  
  //- rjf: setup target environment
  char **envp = 0;
  {
    char **default_env = *_NSGetEnviron();
    U64 default_env_count = 0;
    for(; default_env != 0 && default_env[default_env_count] != 0; default_env_count += 1) {}
    U64 envc = default_env_count + params->env.node_count + 1;
    envp = push_array(scratch.arena, char *, envc);
    MemoryCopyTyped(envp, default_env, default_env_count);
    U64 idx = default_env_count;
    for EachNode(n, String8Node, params->env.first)
    {
      envp[idx] = (char *)push_str8_copy(scratch.arena, n->string).str;
      idx += 1;
    }
  }
  
  //- rjf: spawn the process suspended
  if(argc > 1 && argv[0] != 0)
  {
    posix_spawn_file_actions_t file_actions;
    posix_spawnattr_t attr;
    posix_spawn_file_actions_init(&file_actions);
    posix_spawnattr_init(&attr);
    if(params->path.size != 0)
    {
      posix_spawn_file_actions_addchdir_np(&file_actions, (char *)push_str8_copy(scratch.arena, params->path).str);
    }
    if(!file_match(params->stdout_file, file_zero())) { posix_spawn_file_actions_adddup2(&file_actions, (int)params->stdout_file.u64[0], STDOUT_FILENO); }
    if(!file_match(params->stderr_file, file_zero())) { posix_spawn_file_actions_adddup2(&file_actions, (int)params->stderr_file.u64[0], STDERR_FILENO); }
    if(!file_match(params->stdin_file,  file_zero())) { posix_spawn_file_actions_adddup2(&file_actions, (int)params->stdin_file.u64[0],  STDIN_FILENO); }
    short flags = POSIX_SPAWN_START_SUSPENDED|POSIX_SPAWN_SETSIGMASK;
    sigset_t empty_mask;
    sigemptyset(&empty_mask);
    posix_spawnattr_setsigmask(&attr, &empty_mask);
    posix_spawnattr_setflags(&attr, flags);
    int spawn_code = posix_spawnp(&pid, argv[0], &file_actions, &attr, argv, envp);
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&file_actions);
    if(spawn_code != 0)
    {
      log_infof("mac demon: posix_spawn of '%s' failed (errno %d).\n", argv[0], spawn_code);
      pid = 0;
    }
  }
  
  //- rjf: get task port & attach
  if(pid > 0)
  {
    mach_port_t task = MACH_PORT_NULL;
    kern_return_t kr = task_for_pid(mach_task_self(), pid, &task);
    if(kr != KERN_SUCCESS)
    {
      log_infof("mac demon: task_for_pid(%d) failed (%s). The debugger must be signed with the com.apple.security.cs.debugger entitlement (see build.sh), or run as root.\n", (int)pid, mach_error_string(kr));
      kill(pid, SIGKILL);
      waitpid(pid, 0, 0);
      pid = 0;
    }
    else
    {
      MAC_DMN_Process *process = mac_dmn_process_alloc(pid, task, MAC_DMN_ProcessState_Launch, params->debug_subprocesses, 1);
      mac_dmn_process_install_exception_ports(process);
      if(ptrace(PT_ATTACHEXC, pid, 0, 0) != 0)
      {
        log_infof("mac demon: ptrace(PT_ATTACHEXC, %d) failed (errno %d); signals will not be reported as exceptions.\n", (int)pid, errno);
      }
      // rjf: the process is stopped (start-suspended); we hold it stopped until the
      // first run, which reports its creation.
    }
  }
  
  scratch_end(scratch);
  return (U32)pid;
}

internal B32
dmn_ctrl_attach(DMN_CtrlCtx *ctx, U32 pid)
{
  B32 is_attached = 0;
  mach_port_t task = MACH_PORT_NULL;
  kern_return_t kr = task_for_pid(mach_task_self(), (int)pid, &task);
  if(kr != KERN_SUCCESS)
  {
    log_infof("mac demon: task_for_pid(%d) failed (%s). The debugger must be signed with the com.apple.security.cs.debugger entitlement, or run as root.\n", (int)pid, mach_error_string(kr));
  }
  else
  {
    MAC_DMN_Process *process = mac_dmn_process_alloc((pid_t)pid, task, MAC_DMN_ProcessState_Attach, 1, 0);
    mac_dmn_process_install_exception_ports(process);
    if(ptrace(PT_ATTACHEXC, (pid_t)pid, 0, 0) != 0)
    {
      // rjf: without PT_ATTACHEXC no initial stop arrives and signals would not be
      // reported; treat as a failed attach
      log_infof("mac demon: ptrace(PT_ATTACHEXC, %d) failed (errno %d); cannot attach.\n", (int)pid, errno);
      mac_dmn_process_restore_exception_ports(process);
      mac_dmn_process_release(process);
    }
    else
    {
      is_attached = 1;
    }
  }
  return is_attached;
}

internal B32
dmn_ctrl_kill(DMN_CtrlCtx *ctx, DMN_Handle process_handle, U32 exit_code)
{
  B32 result = 0;
  mutex_take(mac_dmn_state->halter_mutex);
  MAC_DMN_Process *process = mac_dmn_process_from_handle(process_handle);
  if(process != 0 && !process->has_exited)
  {
    result = (kill(process->pid, SIGKILL) == 0);
    // rjf: let the process run so that it can die
    for EachNode(thread, MAC_DMN_Thread, process->first_thread)
    {
      mac_dmn_thread_reply_pending_exception(thread, 1);
      if(thread->is_suspended_by_us)
      {
        thread_resume(thread->port);
        thread->is_suspended_by_us = 0;
      }
    }
    mac_dmn_process_resume(process);
  }
  mutex_drop(mac_dmn_state->halter_mutex);
  return result;
}

internal B32
dmn_ctrl_detach(DMN_CtrlCtx *ctx, DMN_Handle process_handle)
{
  B32 result = 0;
  mutex_take(mac_dmn_state->halter_mutex);
  MAC_DMN_Process *process = mac_dmn_process_from_handle(process_handle);
  if(process != 0)
  {
    if(!process->has_exited)
    {
      mac_dmn_process_set_dyld_notification_trap(process, 0);
      for EachNode(thread, MAC_DMN_Thread, process->first_thread)
      {
        if(thread->is_single_stepping)
        {
          mac_dmn_thread_set_single_step(thread, 0);
        }
        mac_dmn_thread_reply_pending_exception(thread, 1);
        if(thread->is_suspended_by_us)
        {
          thread_resume(thread->port);
          thread->is_suspended_by_us = 0;
        }
      }
      mac_dmn_process_restore_exception_ports(process);
      result = (ptrace(PT_DETACH, process->pid, 0, 0) == 0);
      mac_dmn_process_resume(process);
    }
    mac_dmn_process_release(process);
  }
  mutex_drop(mac_dmn_state->halter_mutex);
  return result;
}

////////////////////////////////
//~ rjf: Run Helpers

internal void
mac_dmn_process_continue_initial(MAC_DMN_Process *process)
{
  for(U64 attempt = 0; attempt < 16; attempt += 1)
  {
    task_basic_info_64_data_t info = {0};
    mach_msg_type_number_t count = TASK_BASIC_INFO_64_COUNT;
    if(task_info(process->task, TASK_BASIC_INFO_64, (task_info_t)&info, &count) != KERN_SUCCESS || info.suspend_count <= 0)
    {
      break;
    }
    if(task_resume(process->task) != KERN_SUCCESS)
    {
      break;
    }
  }
  {
    struct kinfo_proc kp = {0};
    size_t kp_size = sizeof(kp);
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, (int)process->pid};
    if(sysctl(mib, 4, &kp, &kp_size, 0, 0) == 0 && kp_size != 0 && kp.kp_proc.p_stat == SSTOP)
    {
      kill(process->pid, SIGCONT);
    }
  }
}

internal B32
mac_dmn_exception_code_is_repeatable(U64 code)
{
  B32 result = 1;
  for EachNonZeroEnumVal(MAC_ExceptionCodeKind, k)
  {
    if(mac_exception_code_kind_code_table[k] == code)
    {
      result = mac_exception_code_kind_is_repeatable_table[k];
      break;
    }
  }
  return result;
}

internal void
mac_dmn_process_resume_for_run(MAC_DMN_Process *process, B32 ignore_previous_exception)
{
  if(process->has_exited)
  {
    return;
  }
  for EachNode(thread, MAC_DMN_Thread, process->first_thread)
  {
    if(thread->state == MAC_DMN_ThreadState_Exited) { continue; }
    if(thread->is_reg_block_dirty && thread->regs_are_valid)
    {
      thread->is_reg_block_dirty = !mac_dmn_thread_write_reg_block(thread);
    }
    B32 ignore = (ignore_previous_exception || thread->force_ignore_on_reply);
    thread->force_ignore_on_reply = 0;
    mac_dmn_thread_reply_pending_exception(thread, ignore);
    if(!thread->is_suspended_by_us)
    {
      thread->state = MAC_DMN_ThreadState_Running;
      thread->is_reg_block_dirty = 0;
      thread->regs_are_valid = 0;
    }
  }
  mac_dmn_process_resume(process);
  
  // rjf: first resume of a process spawned start-suspended: make sure neither a
  // kernel-side task suspension nor a stopped BSD process state keeps it from running
  if(process->needs_initial_continue)
  {
    process->needs_initial_continue = 0;
    mac_dmn_process_continue_initial(process);
  }
}

internal void
mac_dmn_process_handle_exit(Arena *arena, DMN_EventList *events, MAC_DMN_Process *process, int wait_status, B32 status_is_known)
{
  process->has_exited = 1;
  U64 exit_code = 0;
  if(status_is_known)
  {
    if(WIFEXITED(wait_status))        { exit_code = (U64)WEXITSTATUS(wait_status); }
    else if(WIFSIGNALED(wait_status)) { exit_code = (U64)WTERMSIG(wait_status); }
  }
  if(process->is_child)
  {
    int status = 0;
    pid_t wait_result = 0;
    for(U64 attempt = 0; attempt < 200; attempt += 1)
    {
      wait_result = waitpid(process->pid, &status, WNOHANG);
      if(wait_result == process->pid || (wait_result < 0 && errno != EINTR)) { break; }
      usleep(1000);
    }
    if(wait_result == process->pid && !status_is_known)
    {
      if(WIFEXITED(status))        { exit_code = (U64)WEXITSTATUS(status); }
      else if(WIFSIGNALED(status)) { exit_code = (U64)WTERMSIG(status); }
    }
  }
  process->exit_code = exit_code;
  
  // rjf: thread exit events
  for EachNode(thread, MAC_DMN_Thread, process->first_thread)
  {
    thread->state = MAC_DMN_ThreadState_Exited;
    thread->has_pending_exception = 0;
    DMN_Event *e = dmn_event_list_push(arena, events);
    e->kind    = DMN_EventKind_ExitThread;
    e->process = mac_dmn_handle_from_process(process);
    e->thread  = mac_dmn_handle_from_thread(thread);
    e->code    = (U32)exit_code;
  }
  
  // rjf: module unload events
  if(process->ctx != 0)
  {
    for(MAC_DMN_Module *module = process->ctx->first_module; module != 0; module = module->order_next)
    {
      mac_dmn_push_event_unload_module(arena, events, process, module);
    }
  }
  
  // rjf: process exit event
  {
    DMN_Event *e = dmn_event_list_push(arena, events);
    e->kind    = DMN_EventKind_ExitProcess;
    e->process = mac_dmn_handle_from_process(process);
    e->code    = (U32)exit_code;
  }
  
  // rjf: release
  mac_dmn_process_release(process);
}

internal void
mac_dmn_process_handle_dyld_notification(Arena *arena, DMN_EventList *events, MAC_DMN_Process *process, MAC_DMN_Thread *thread)
{
  Temp scratch = scratch_begin(&arena, 1);
  ARM64_RegBlock *regs = (ARM64_RegBlock *)thread->reg_block;
  // NOTE: mode and count are 32-bit arguments; the upper halves of the registers are unspecified
  U64 mode  = (U32)regs->x0;
  U64 count = (U32)regs->x1;
  U64 array_vaddr = regs->x2;
  
  //- rjf: dyld_image_dyld_moved: dyld transferred control to the copy of itself in
  // the dyld shared cache; the old image (and our trap in it) is dead. re-target.
  if(mode == 3 && count != 0 && array_vaddr != 0)
  {
    struct dyld_image_info moved = {0};
    if(mac_dmn_read_struct(process->task, array_vaddr, &moved) == sizeof(moved) && moved.imageLoadAddress != 0)
    {
      U64 new_dyld_vaddr = (U64)(uintptr_t)moved.imageLoadAddress;
      String8 path = mac_dmn_read_string_capped(scratch.arena, process->task, (U64)(uintptr_t)moved.imageFilePath, 4096);
      if(path.size == 0) { path = str8_lit("/usr/lib/dyld"); }
      // rjf: finish executing the old trap site first, then remove it for good
      mac_dmn_thread_step_over_internal(process, thread);
      mac_dmn_process_set_dyld_notification_trap(process, 0);
      process->dyld_notification_vaddr = 0;
      // rjf: unload the old dyld module, load the new one
      for(MAC_DMN_Module *m = process->ctx->first_module, *next = 0; m != 0; m = next)
      {
        next = m->order_next;
        if(m->is_dyld)
        {
          mac_dmn_push_event_unload_module(arena, events, process, m);
          mac_dmn_module_release(process->ctx, m);
        }
      }
      mac_dmn_push_event_load_module(arena, events, process, thread, new_dyld_vaddr, path, 0, 1);
      // rjf: re-arm the notification trap in the new dyld (it has already rebased itself)
      if(mac_dmn_process_find_dyld_notification(process, new_dyld_vaddr, 1))
      {
        mac_dmn_process_set_dyld_notification_trap(process, 1);
      }
    }
    scratch_end(scratch);
    return;
  }
  
  if(count != 0 && count < 65536 && array_vaddr != 0)
  {
    struct dyld_image_info *array = push_array_no_zero(scratch.arena, struct dyld_image_info, count);
    U64 array_read = mac_dmn_read(process->task, r1u64(array_vaddr, array_vaddr + count*sizeof(array[0])), array);
    count = array_read / sizeof(array[0]);
    for EachIndex(idx, count)
    {
      U64 header_vaddr = (U64)(uintptr_t)array[idx].imageLoadAddress;
      if(mode == 0) // rjf: dyld_image_adding
      {
        String8 path = mac_dmn_read_string_capped(scratch.arena, process->task, (U64)(uintptr_t)array[idx].imageFilePath, 4096);
        struct mach_header_64 hdr = {0};
        B32 is_main = (mac_dmn_read_struct(process->task, header_vaddr, &hdr) == sizeof(hdr) && hdr.filetype == MH_EXECUTE);
        mac_dmn_push_event_load_module(arena, events, process, thread, header_vaddr, path, is_main, 0);
      }
      else if(mode == 1) // rjf: dyld_image_removing
      {
        MAC_DMN_Module *module = mac_dmn_module_from_base_vaddr(process->ctx, header_vaddr);
        if(module != 0)
        {
          mac_dmn_push_event_unload_module(arena, events, process, module);
          mac_dmn_module_release(process->ctx, module);
        }
      }
    }
  }
  scratch_end(scratch);
}

internal DMN_ExceptionKind
mac_dmn_exception_kind_from_thread(MAC_DMN_Thread *thread)
{
  DMN_ExceptionKind kind = DMN_ExceptionKind_Null;
  arm_exception_state64_t es = {0};
  mach_msg_type_number_t count = ARM_EXCEPTION_STATE64_COUNT;
  if(thread_get_state(thread->port, ARM_EXCEPTION_STATE64, (thread_state_t)&es, &count) == KERN_SUCCESS)
  {
    ARM64_ESR_EC ec = ARM64_ESR_EC_FROM_ESR(es.__esr);
    switch(ec)
    {
      default:{}break;
      case ARM64_ESR_EC_DataAbortLower:
      case ARM64_ESR_EC_DataAbortCurrent:
      {
        kind = (ARM64_ESR_ISS_FROM_ESR(es.__esr) & ARM64_ESR_DataAbort_WnR) ? DMN_ExceptionKind_MemoryWrite : DMN_ExceptionKind_MemoryRead;
      }break;
      case ARM64_ESR_EC_InstAbortLower:
      case ARM64_ESR_EC_InstAbortCurrent:
      {
        kind = DMN_ExceptionKind_MemoryExecute;
      }break;
    }
  }
  return kind;
}

////////////////////////////////
//~ rjf: @dmn_os_hooks Run

internal DMN_EventList
dmn_ctrl_run(Arena *arena, DMN_CtrlCtx *ctx, DMN_RunCtrls *ctrls)
{
  Temp scratch = scratch_begin(&arena, 1);
  DMN_EventList events = {0};
  
  //////////////////////////////
  //- rjf: prevent halts temporarily, while debuggee state can change
  //
  mutex_take(mac_dmn_state->halter_mutex);
  
  //////////////////////////////
  //- rjf: report newly launched processes; they are stopped at the first
  // instruction of dyld, and stay stopped until the next run.
  //
  for(MAC_DMN_Process *process = mac_dmn_state->first_process, *next = 0; process != 0; process = next)
  {
    next = process->next;
    if(process->state == MAC_DMN_ProcessState_Launch)
    {
      if(process->task_suspend_count == 0)
      {
        mac_dmn_process_suspend(process);
      }
      process->needs_initial_continue = 1;
      if(!mac_dmn_process_init_ctx(arena, &events, process, 1))
      {
        kill(process->pid, SIGKILL);
        mac_dmn_process_resume(process);
        mac_dmn_process_continue_initial(process);
        for(U64 attempt = 0; attempt < 200; attempt += 1)
        {
          pid_t r = waitpid(process->pid, 0, WNOHANG);
          if(r == process->pid || (r < 0 && errno != EINTR)) { break; }
          usleep(1000);
        }
        mac_dmn_process_release(process);
      }
    }
  }
  
  //////////////////////////////
  //- rjf: run, if there is something to run, and no creation events to report
  //
  if(events.count == 0 && mac_dmn_state->process_count > 0)
  {
    ARCH_Info *arch_info = arch_info_from_arch(Arch_arm64);
    U64 bytes_per_trap = arch_info->trap_instruction.size;
    
    ////////////////////////////
    //- rjf: read initial bytes at all trap locations
    //
    // NOTE: all of the original bytes must be read before any trap is written:
    // the list may contain several traps at one address (e.g. a stepping trap
    // on the same instruction as a user breakpoint), and reading after writing
    // would record the trap instruction as that address' original bytes, which
    // the restore below would then leave permanently in the program.
    //
    U8 *trap_swap_bytes = push_array(scratch.arena, U8, ctrls->traps.trap_count * bytes_per_trap);
    B8 *trap_is_read    = push_array(scratch.arena, B8, ctrls->traps.trap_count);
    B8 *trap_is_written = push_array(scratch.arena, B8, ctrls->traps.trap_count);
    {
      U64 trap_idx = 0;
      for EachNode(n, DMN_TrapChunkNode, ctrls->traps.first)
      {
        for EachIndex(n_idx, n->count)
        {
          DMN_Trap *trap = n->v+n_idx;
          if(trap->flags == 0)
          {
            U8 *swap = trap_swap_bytes + trap_idx*bytes_per_trap;
            trap_is_read[trap_idx] = (dmn_process_read(trap->process, r1u64(trap->vaddr, trap->vaddr + bytes_per_trap), swap) == bytes_per_trap);
          }
          trap_idx += 1;
        }
      }
    }
    
    ////////////////////////////
    //- rjf: write trap instructions
    //
    {
      U64 trap_idx = 0;
      for EachNode(n, DMN_TrapChunkNode, ctrls->traps.first)
      {
        for EachIndex(n_idx, n->count)
        {
          DMN_Trap *trap = n->v+n_idx;
          if(trap->flags == 0 && trap_is_read[trap_idx])
          {
            trap_is_written[trap_idx] = dmn_process_write(trap->process, r1u64(trap->vaddr, trap->vaddr + bytes_per_trap), arch_info->trap_instruction.str);
          }
          trap_idx += 1;
        }
      }
    }
    
    ////////////////////////////
    //- rjf: program hardware traps (watchpoints)
    //
    for EachNode(process, MAC_DMN_Process, mac_dmn_state->first_process)
    {
      mac_dmn_process_set_hw_traps(process, &ctrls->traps, 1);
    }
    
    ////////////////////////////
    //- rjf: enable single stepping
    //
    if(!dmn_handle_match(ctrls->single_step_thread, dmn_handle_zero()))
    {
      MAC_DMN_Thread *single_step_thread = mac_dmn_thread_from_handle(ctrls->single_step_thread);
      if(single_step_thread != 0)
      {
        mac_dmn_thread_set_single_step(single_step_thread, 1);
      }
    }
    
    ////////////////////////////
    //- rjf: freeze / unfreeze threads, then resume all processes
    //
    for EachNode(process, MAC_DMN_Process, mac_dmn_state->first_process)
    {
      if(process->has_exited) { continue; }
      
      // rjf: determine if this process is frozen
      B32 process_is_frozen = 0;
      if(ctrls->run_entities_are_processes)
      {
        for EachIndex(idx, ctrls->run_entity_count)
        {
          if(dmn_handle_match(ctrls->run_entities[idx], mac_dmn_handle_from_process(process)))
          {
            process_is_frozen = 1;
            break;
          }
        }
      }
      
      // rjf: apply per-thread freezing
      for EachNode(thread, MAC_DMN_Thread, process->first_thread)
      {
        if(thread->state == MAC_DMN_ThreadState_Exited) { continue; }
        B32 is_frozen = 0;
        if(dmn_handle_match(dmn_handle_zero(), ctrls->single_step_thread))
        {
          if(ctrls->run_entities_are_processes)
          {
            is_frozen = process_is_frozen;
          }
          else
          {
            for EachIndex(idx, ctrls->run_entity_count)
            {
              if(dmn_handle_match(ctrls->run_entities[idx], mac_dmn_handle_from_thread(thread)))
              {
                is_frozen = 1;
                break;
              }
            }
          }
          if(ctrls->run_entities_are_unfrozen)
          {
            is_frozen ^= 1;
          }
        }
        else
        {
          is_frozen = !dmn_handle_match(mac_dmn_handle_from_thread(thread), ctrls->single_step_thread);
        }
        if(is_frozen && !thread->is_suspended_by_us)
        {
          if(thread_suspend(thread->port) == KERN_SUCCESS)
          {
            thread->is_suspended_by_us = 1;
          }
        }
        else if(!is_frozen && thread->is_suspended_by_us)
        {
          thread_resume(thread->port);
          thread->is_suspended_by_us = 0;
        }
      }
      
      // rjf: resume
      mac_dmn_process_resume_for_run(process, ctrls->ignore_previous_exception);
    }
    
    ////////////////////////////
    //- rjf: wait for stops & generate events
    //
    B32 is_halt_done = 0;
    for(;;)
    {
      //////////////////////////
      //- rjf: wait for a kernel event (unblock halts)
      //
      struct kevent kev = {0};
      int kev_count = 0;
      {
        struct timespec timeout = {0, 100*1000*1000};
        mutex_drop(mac_dmn_state->halter_mutex);
        kev_count = kevent(mac_dmn_state->kq, 0, 0, &kev, 1, &timeout);
        mutex_take(mac_dmn_state->halter_mutex);
      }
      if(kev_count < 0 && errno != EINTR)
      {
        log_infof("kqueue error: kevent failed (errno %d).\n", errno);
        DMN_Event *e = dmn_event_list_push(arena, &events);
        e->kind = DMN_EventKind_Error;
        e->error_kind = DMN_ErrorKind_UnexpectedFailure;
        break;
      }
      
      B32 got_stop = 0;
      
      //////////////////////////
      //- rjf: halt requests
      //
      if(mac_dmn_state->halt_requested)
      {
        mac_dmn_state->halt_requested = 0;
        is_halt_done = 1;
        got_stop = 1;
      }
      
      //////////////////////////
      //- rjf: process exits
      //
      if(kev_count > 0 && kev.filter == EVFILT_PROC && (kev.fflags & NOTE_EXIT))
      {
        MAC_DMN_Process *process = mac_dmn_process_from_pid((pid_t)kev.ident);
        if(process != 0 && !process->has_exited)
        {
          mac_dmn_process_handle_exit(arena, &events, process, (int)kev.data, !!(kev.fflags & NOTE_EXITSTATUS));
          got_stop = 1;
        }
      }
      
      //////////////////////////
      //- rjf: exception messages (drained on every wakeup)
      //
      if(mac_dmn_process_receive_exceptions(0, 0))
      {
        got_stop = 1;
      }
      
      //////////////////////////
      //- rjf: liveness safety net
      //
      if(kev_count == 0)
      {
        for(MAC_DMN_Process *process = mac_dmn_state->first_process, *next = 0; process != 0; process = next)
        {
          next = process->next;
          if(!process->has_exited && kill(process->pid, 0) != 0 && errno == ESRCH)
          {
            mac_dmn_process_handle_exit(arena, &events, process, 0, 0);
            got_stop = 1;
          }
        }
      }
      
      //////////////////////////
      //- rjf: no stop yet -> keep waiting
      //
      if(!got_stop)
      {
        continue;
      }
      
      //////////////////////////
      //- rjf: stop everything
      //
      for EachNode(process, MAC_DMN_Process, mac_dmn_state->first_process)
      {
        if(!process->has_exited && process->task_suspend_count == 0)
        {
          mac_dmn_process_suspend(process);
        }
      }
      for(U64 drain_idx = 0; drain_idx < 2; drain_idx += 1)
      {
        mac_dmn_process_receive_exceptions(0, 2);
      }
      
      //////////////////////////
      //- rjf: refresh thread lists & registers of all stopped threads
      //
      for EachNode(process, MAC_DMN_Process, mac_dmn_state->first_process)
      {
        if(process->has_exited || process->ctx == 0) { continue; }
        mac_dmn_process_sync_threads(arena, &events, process, (process->state == MAC_DMN_ProcessState_Normal));
        mac_dmn_process_check_images(arena, &events, process);
        for EachNode(thread, MAC_DMN_Thread, process->first_thread)
        {
          if(thread->state == MAC_DMN_ThreadState_Running)
          {
            thread->state = MAC_DMN_ThreadState_Stopped;
            thread->is_reg_block_dirty = 0;
            mac_dmn_thread_read_reg_block(thread);
          }
          else if(thread->state == MAC_DMN_ThreadState_Stopped && !thread->regs_are_valid)
          {
            mac_dmn_thread_read_reg_block(thread);
          }
        }
      }
      
      //////////////////////////
      //- rjf: generate events for threads which stopped with exceptions
      //
      for(MAC_DMN_Process *process = mac_dmn_state->first_process, *next_process = 0; process != 0; process = next_process)
      {
        next_process = process->next;
        if(process->has_exited || process->ctx == 0) { continue; }
        DMN_Handle process_handle = mac_dmn_handle_from_process(process);
        for EachNode(thread, MAC_DMN_Thread, process->first_thread)
        {
          if(!thread->has_pending_exception || thread->stop_reported) { continue; }
          thread->stop_reported = 1;
          DMN_Handle thread_handle = mac_dmn_handle_from_thread(thread);
          U64 ip = mac_dmn_ip_from_thread(thread);
          B32 is_exception = 0;
          U64 exception_code = 0;
          U64 exception_address = ip;
          DMN_ExceptionKind exception_kind = DMN_ExceptionKind_Null;
          switch(thread->stop_kind)
          {
            default:{}break;
            
            ////////////////////
            //- rjf: brk hits
            //
            case MAC_DMN_StopKind_Breakpoint:
            {
              // rjf: find associated user trap (the dyld notification trap takes precedence)
              DMN_Trap *hit_user_trap = 0;
              B32 is_dyld_trap = (process->dyld_notification_trap_is_set && ip == process->dyld_notification_vaddr);
              if(!is_dyld_trap)
              {
                U64 trap_idx = 0;
                for EachNode(n, DMN_TrapChunkNode, ctrls->traps.first)
                {
                  for EachIndex(n_idx, n->count)
                  {
                    DMN_Trap *trap = n->v+n_idx;
                    if(trap->flags == 0 && trap_is_written[trap_idx] && dmn_handle_match(trap->process, process_handle) && trap->vaddr == ip)
                    {
                      hit_user_trap = trap;
                      goto dbl_break_find_user_trap;
                    }
                    trap_idx += 1;
                  }
                }
                dbl_break_find_user_trap:;
              }
              
              // rjf: user trap -> breakpoint event (pc already points at the trap instruction)
              if(hit_user_trap != 0)
              {
                DMN_Event *e = dmn_event_list_push(arena, &events);
                e->kind                = DMN_EventKind_Breakpoint;
                e->process             = process_handle;
                e->thread              = thread_handle;
                e->instruction_pointer = ip;
                e->user_data           = hit_user_trap->id;
                thread->stop_kind = MAC_DMN_StopKind_Suspended;
              }
              
              // rjf: dyld notification trap -> module events, step over internally
              else if(is_dyld_trap)
              {
                mac_dmn_process_handle_dyld_notification(arena, &events, process, thread);
                mac_dmn_thread_step_over_internal(process, thread);
              }
              
              // rjf: foreign brk (e.g. __builtin_trap) -> exception
              else
              {
                is_exception = 1;
                exception_code = MAC_DMN_EXC_CODE_BASE + EXC_BREAKPOINT;
                thread->stop_kind = MAC_DMN_StopKind_HardException;
              }
            }break;
            
            ////////////////////
            //- rjf: single-step completions
            //
            case MAC_DMN_StopKind_SingleStep:
            {
              mac_dmn_thread_set_single_step(thread, 0);
              DMN_Event *e = dmn_event_list_push(arena, &events);
              e->kind                = DMN_EventKind_SingleStep;
              e->process             = process_handle;
              e->thread              = thread_handle;
              e->instruction_pointer = ip;
              e->address             = ip;
              thread->stop_kind = MAC_DMN_StopKind_Suspended;
            }break;
            
            ////////////////////
            //- rjf: watchpoints
            //
            case MAC_DMN_StopKind_Watchpoint:
            {
              U64 address = thread->stop_hw_trap_vaddr;
              for EachNode(n, DMN_TrapChunkNode, ctrls->traps.first)
              {
                for EachIndex(n_idx, n->count)
                {
                  DMN_Trap *trap = n->v+n_idx;
                  U64 size = trap->size ? trap->size : 8;
                  if(trap->flags != 0 && dmn_handle_match(trap->process, process_handle) && contains_1u64(r1u64(trap->vaddr, trap->vaddr+size), address))
                  {
                    address = trap->vaddr;
                  }
                }
              }
              DMN_Event *e = dmn_event_list_push(arena, &events);
              e->kind                = DMN_EventKind_Breakpoint;
              e->process             = process_handle;
              e->thread              = thread_handle;
              e->instruction_pointer = ip;
              e->address             = address;
              // rjf: arm64 watchpoints fire before the access; step past it now
              mac_dmn_thread_step_over_internal(process, thread);
            }break;
            
            ////////////////////
            //- rjf: soft signals
            //
            case MAC_DMN_StopKind_SoftSignal:
            {
              U64 signo = thread->stop_signo;
              if(signo == SIGSTOP || signo == SIGCONT)
              {
                thread->force_ignore_on_reply = 1;
                if(process->state == MAC_DMN_ProcessState_Attach)
                {
                  if(!mac_dmn_process_init_ctx(arena, &events, process, 0))
                  {
                    kill(process->pid, SIGKILL);
                  }
                }
              }
              else
              {
                is_exception = 1;
                exception_code = signo;
                if(signo == SIGSEGV || signo == SIGBUS)
                {
                  exception_kind = mac_dmn_exception_kind_from_thread(thread);
                }
              }
            }break;
            
            ////////////////////
            //- rjf: hardware (mach) exceptions
            //
            case MAC_DMN_StopKind_HardException:
            {
              MAC_DMN_ExceptionRaiseRequest *req = &thread->pending_exception;
              is_exception = 1;
              exception_code = MAC_DMN_EXC_CODE_BASE + (U64)req->exception;
              if(req->exception == EXC_BAD_ACCESS)
              {
                if(req->code_count > 1) { exception_address = (U64)req->code[1]; }
                exception_kind = mac_dmn_exception_kind_from_thread(thread);
              }
            }break;
          }
          
          //- rjf: report exception
          if(is_exception)
          {
            B32 is_repeatable = mac_dmn_exception_code_is_repeatable(exception_code);
            B32 is_second_chance = (is_repeatable &&
                                    thread->last_exception_ip == ip &&
                                    thread->last_exception_code == exception_code);
            // rjf: a signal produced by escalating a hardware exception at this ip is its second chance
            if(thread->escalated_exception && thread->escalated_ip == ip && thread->stop_kind == MAC_DMN_StopKind_SoftSignal)
            {
              is_second_chance = 1;
            }
            thread->escalated_exception = 0;
            if(is_repeatable)
            {
              thread->last_exception_ip = ip;
              thread->last_exception_code = exception_code;
            }
            DMN_Event *e = dmn_event_list_push(arena, &events);
            e->kind                = DMN_EventKind_Exception;
            e->exception_kind      = exception_kind;
            e->process             = process_handle;
            e->thread              = thread_handle;
            e->instruction_pointer = ip;
            e->address             = exception_address;
            e->code                = (U32)exception_code;
            e->exception_repeated  = is_second_chance;
          }
          else
          {
            thread->last_exception_ip = 0;
            thread->last_exception_code = 0;
            thread->escalated_exception = 0;
          }
        }
      }
      
      //////////////////////////
      //- rjf: nothing to report -> resume & keep waiting
      //
      if(events.count == 0 && !is_halt_done)
      {
        for EachNode(process, MAC_DMN_Process, mac_dmn_state->first_process)
        {
          for EachNode(thread, MAC_DMN_Thread, process->first_thread)
          {
            thread->stop_reported = 0;
          }
          mac_dmn_process_resume_for_run(process, 0);
        }
        continue;
      }
      break;
    }
    
    ////////////////////////////
    //- rjf: halted -> push halt event, reset halter state
    //
    if(is_halt_done)
    {
      DMN_Event *e = dmn_event_list_push(arena, &events);
      e->kind = DMN_EventKind_Halt;
      mac_dmn_state->halt_code      = 0;
      mac_dmn_state->halt_user_data = 0;
    }
    
    ////////////////////////////
    //- rjf: clear stop-reported marks, single stepping, hardware traps
    //
    for EachNode(process, MAC_DMN_Process, mac_dmn_state->first_process)
    {
      for EachNode(thread, MAC_DMN_Thread, process->first_thread)
      {
        thread->stop_reported = 0;
        if(thread->is_single_stepping)
        {
          mac_dmn_thread_set_single_step(thread, 0);
        }
      }
      mac_dmn_process_set_hw_traps(process, &ctrls->traps, 0);
    }
    
    ////////////////////////////
    //- rjf: unset all traps - restore original bytes
    //
    {
      U64 trap_idx = 0;
      for EachNode(n, DMN_TrapChunkNode, ctrls->traps.first)
      {
        for EachIndex(n_idx, n->count)
        {
          DMN_Trap *trap = n->v+n_idx;
          if(trap->flags == 0 && trap_is_written[trap_idx])
          {
            dmn_process_write(trap->process, r1u64(trap->vaddr, trap->vaddr + bytes_per_trap), trap_swap_bytes + trap_idx*bytes_per_trap);
          }
          trap_idx += 1;
        }
      }
    }
  }
  
  //////////////////////////////
  //- rjf: no events, no processes -> not attached
  //
  if(events.count == 0 && mac_dmn_state->process_count == 0)
  {
    DMN_Event *e = dmn_event_list_push(arena, &events);
    e->kind = DMN_EventKind_Error;
    e->error_kind = DMN_ErrorKind_NotAttached;
  }
  
  //////////////////////////////
  //- rjf: gather thread names
  //
  for EachNode(process, MAC_DMN_Process, mac_dmn_state->first_process)
  {
    if(process->has_exited) { continue; }
    for EachNode(thread, MAC_DMN_Thread, process->first_thread)
    {
      if(thread->last_name_hash == 0 || thread->name_gather_time_us + 1000000 <= now_time_us())
      {
        thread_extended_info_data_t ext_info = {0};
        mach_msg_type_number_t count = THREAD_EXTENDED_INFO_COUNT;
        if(thread_info(thread->port, THREAD_EXTENDED_INFO, (thread_info_t)&ext_info, &count) == KERN_SUCCESS)
        {
          String8 thread_name = str8_cstring_capped(ext_info.pth_name, ext_info.pth_name + sizeof(ext_info.pth_name));
          U64 hash = u64_hash_from_str8(thread_name);
          if(thread_name.size != 0 && hash != thread->last_name_hash)
          {
            DMN_Event *e = dmn_event_list_push(arena, &events);
            e->kind    = DMN_EventKind_SetThreadName;
            e->process = mac_dmn_handle_from_process(process);
            e->thread  = mac_dmn_handle_from_thread(thread);
            e->string  = push_str8_copy(arena, thread_name);
          }
          thread->last_name_hash = hash ? hash : 1;
        }
        thread->name_gather_time_us = now_time_us();
      }
    }
  }
  
  //////////////////////////////
  //- rjf: re-allow halts
  //
  mutex_drop(mac_dmn_state->halter_mutex);
  
  scratch_end(scratch);
  return events;
}

////////////////////////////////
//~ rjf: @dmn_os_hooks Halting (Implemented Per-OS)

internal void
dmn_halt(U64 code, U64 user_data)
{
  MutexScope(mac_dmn_state->halter_mutex)
  {
    if(mac_dmn_state->process_count)
    {
      mac_dmn_state->halt_requested = 1;
      mac_dmn_state->halt_code      = code;
      mac_dmn_state->halt_user_data = user_data;
      struct kevent kev = {0};
      EV_SET(&kev, 1, EVFILT_USER, 0, NOTE_TRIGGER, 0, 0);
      kevent(mac_dmn_state->kq, &kev, 1, 0, 0, 0);
    }
  }
}

////////////////////////////////
//~ rjf: @dmn_os_hooks Introspection Functions (Implemented Per-OS)

//- rjf: non-blocking-control-thread access barriers

internal B32
dmn_access_open(void)
{
  B32 result = 0;
  if(mac_dmn_ctrl_thread)
  {
    result = 1;
  }
  else
  {
    mutex_take(mac_dmn_state->access_mutex);
    result = !mac_dmn_state->access_run_state;
  }
  return result;
}

internal void
dmn_access_close(void)
{
  if(!mac_dmn_ctrl_thread)
  {
    mutex_drop(mac_dmn_state->access_mutex);
  }
}

//- rjf: processes

internal U64
dmn_process_memory_reserve(DMN_Handle process_handle, U64 vaddr, U64 size)
{
  U64 result = 0;
  DMN_AccessScope
  {
    MAC_DMN_Process *process = mac_dmn_process_from_handle(process_handle);
    if(process != 0 && !process->has_exited)
    {
      mach_vm_address_t address = (mach_vm_address_t)vaddr;
      kern_return_t kr = mach_vm_allocate(process->task, &address, (mach_vm_size_t)size, vaddr == 0 ? VM_FLAGS_ANYWHERE : VM_FLAGS_FIXED);
      if(kr == KERN_SUCCESS)
      {
        mach_vm_protect(process->task, address, (mach_vm_size_t)size, 0, VM_PROT_NONE);
        result = (U64)address;
      }
    }
  }
  return result;
}

internal void
dmn_process_memory_commit(DMN_Handle process_handle, U64 vaddr, U64 size)
{
  DMN_AccessScope
  {
    MAC_DMN_Process *process = mac_dmn_process_from_handle(process_handle);
    if(process != 0 && !process->has_exited)
    {
      mach_vm_protect(process->task, (mach_vm_address_t)vaddr, (mach_vm_size_t)size, 0, VM_PROT_READ|VM_PROT_WRITE);
    }
  }
}

internal void
dmn_process_memory_decommit(DMN_Handle process_handle, U64 vaddr, U64 size)
{
  DMN_AccessScope
  {
    MAC_DMN_Process *process = mac_dmn_process_from_handle(process_handle);
    if(process != 0 && !process->has_exited)
    {
      mach_vm_protect(process->task, (mach_vm_address_t)vaddr, (mach_vm_size_t)size, 0, VM_PROT_NONE);
    }
  }
}

internal void
dmn_process_memory_release(DMN_Handle process_handle, U64 vaddr, U64 size)
{
  DMN_AccessScope
  {
    MAC_DMN_Process *process = mac_dmn_process_from_handle(process_handle);
    if(process != 0 && !process->has_exited)
    {
      mach_vm_deallocate(process->task, (mach_vm_address_t)vaddr, (mach_vm_size_t)size);
    }
  }
}

internal void
dmn_process_memory_protect(DMN_Handle process_handle, U64 vaddr, U64 size, AccessFlags flags)
{
  DMN_AccessScope
  {
    MAC_DMN_Process *process = mac_dmn_process_from_handle(process_handle);
    if(process != 0 && !process->has_exited)
    {
      vm_prot_t prot = VM_PROT_NONE;
      if(flags & AccessFlag_Read)    { prot |= VM_PROT_READ; }
      if(flags & AccessFlag_Write)   { prot |= VM_PROT_WRITE; }
      if(flags & AccessFlag_Execute) { prot |= VM_PROT_EXECUTE; }
      mach_vm_protect(process->task, (mach_vm_address_t)vaddr, (mach_vm_size_t)size, 0, prot);
    }
  }
}

internal U64
dmn_process_read(DMN_Handle process_handle, Rng1U64 range, void *dst)
{
  U64 result = 0;
  DMN_AccessScope
  {
    MAC_DMN_Process *process = mac_dmn_process_from_handle(process_handle);
    if(process != 0 && !process->has_exited)
    {
      result = mac_dmn_read(process->task, range, dst);
    }
  }
  return result;
}

internal B32
dmn_process_write(DMN_Handle process_handle, Rng1U64 range, void *src)
{
  B32 result = 0;
  DMN_AccessScope
  {
    MAC_DMN_Process *process = mac_dmn_process_from_handle(process_handle);
    if(process != 0 && !process->has_exited)
    {
      result = mac_dmn_write(process->task, range, src);
    }
  }
  return result;
}

//- rjf: threads

internal B32
dmn_thread_read_reg_block(DMN_Handle thread_handle, void *reg_block)
{
  B32 result = 0;
  DMN_AccessScope
  {
    MAC_DMN_Thread *thread = mac_dmn_thread_from_handle(thread_handle);
    if(thread != 0 && thread->process->ctx != 0)
    {
      ARCH_Info *arch_info = arch_info_from_arch(thread->process->ctx->arch);
      MemoryCopy(reg_block, thread->reg_block, arch_info->reg_block_size);
      result = 1;
    }
  }
  return result;
}

internal B32
dmn_thread_write_reg_block(DMN_Handle thread_handle, void *reg_block)
{
  B32 result = 0;
  DMN_AccessScope
  {
    MAC_DMN_Thread *thread = mac_dmn_thread_from_handle(thread_handle);
    if(thread != 0 && thread->process->ctx != 0)
    {
      ARCH_Info *arch_info = arch_info_from_arch(thread->process->ctx->arch);
      MemoryCopy(thread->reg_block, reg_block, arch_info->reg_block_size);
      thread->is_reg_block_dirty = 1;
      thread->regs_are_valid = 1;
      result = 1;
    }
  }
  return result;
}

internal B32
dmn_thread_get_module_tls_vaddr(DMN_Handle thread_handle, DMN_Handle module_handle, U64 *vaddr_out)
{
  // NOTE: on macOS every image with thread-local variables owns a pthread
  // key, stored in each of its TLV descriptors (__DATA,__thread_vars:
  // {thunk, key, offset}). A thread's TLS block for the image is found via
  // its TSD base (TPIDRRO_EL0): block = *(tsd_base + key*8). We return that
  // block base; variable offsets within the block come from the descriptors.
  B32 result = 0;
  DMN_AccessScope
  {
    MAC_DMN_Thread *thread = mac_dmn_thread_from_handle(thread_handle);
    MAC_DMN_Module *module = mac_dmn_module_from_handle(module_handle);
    if(thread != 0 && module != 0 && !thread->process->has_exited)
    {
      MAC_DMN_Process *process = thread->process;
      if(module->tls_key == 0 && module->thread_vars_vaddr != 0)
      {
        U64 key = 0;
        if(mac_dmn_read_struct(process->task, module->thread_vars_vaddr + 8, &key) == sizeof(key))
        {
          module->tls_key = key;
        }
      }
      if(thread->tsd_base_vaddr == 0)
      {
        mac_dmn_thread_refresh_identifier_info(thread);
      }
      if(module->tls_key != 0 && thread->tsd_base_vaddr != 0)
      {
        U64 block_vaddr = 0;
        U64 slot_vaddr = thread->tsd_base_vaddr + module->tls_key*8;
        if(mac_dmn_read_struct(process->task, slot_vaddr, &block_vaddr) == sizeof(block_vaddr) && block_vaddr != 0)
        {
          vaddr_out[0] = block_vaddr;
          result = 1;
        }
      }
    }
  }
  return result;
}

//- rjf: system process listing

typedef struct MAC_DMN_ProcessIterState MAC_DMN_ProcessIterState;
struct MAC_DMN_ProcessIterState
{
  pid_t *pids;
  U64 count;
  U64 idx;
};

internal void
dmn_process_iter_begin(DMN_ProcessIter *iter)
{
  MemoryZeroStruct(iter);
  int bytes = proc_listpids(PROC_ALL_PIDS, 0, 0, 0);
  if(bytes > 0)
  {
    MAC_DMN_ProcessIterState *state = (MAC_DMN_ProcessIterState *)malloc(sizeof(MAC_DMN_ProcessIterState));
    MemoryZeroStruct(state);
    state->pids = (pid_t *)malloc((size_t)bytes);
    int got = proc_listpids(PROC_ALL_PIDS, 0, state->pids, bytes);
    state->count = (got > 0) ? (U64)got / sizeof(pid_t) : 0;
    iter->v[0] = IntFromPtr(state);
  }
}

internal B32
dmn_process_iter_next(Arena *arena, DMN_ProcessIter *iter, DMN_ProcessInfo *info_out)
{
  B32 result = 0;
  MAC_DMN_ProcessIterState *state = (MAC_DMN_ProcessIterState *)PtrFromInt(iter->v[0]);
  if(state != 0)
  {
    for(;state->idx < state->count;)
    {
      pid_t pid = state->pids[state->idx];
      state->idx += 1;
      if(pid <= 0) { continue; }
      char path_buffer[PROC_PIDPATHINFO_MAXSIZE] = {0};
      int path_size = proc_pidpath(pid, path_buffer, sizeof(path_buffer));
      String8 name = {0};
      if(path_size > 0)
      {
        name = push_str8_copy(arena, str8((U8 *)path_buffer, (U64)path_size));
      }
      else
      {
        char name_buffer[256] = {0};
        int name_size = proc_name(pid, name_buffer, sizeof(name_buffer));
        if(name_size > 0)
        {
          name = push_str8_copy(arena, str8((U8 *)name_buffer, (U64)name_size));
        }
      }
      if(name.size == 0)
      {
        name = str8_lit("(unknown process)");
      }
      info_out->name = name;
      info_out->pid = (U32)pid;
      result = 1;
      break;
    }
  }
  return result;
}

internal void
dmn_process_iter_end(DMN_ProcessIter *iter)
{
  MAC_DMN_ProcessIterState *state = (MAC_DMN_ProcessIterState *)PtrFromInt(iter->v[0]);
  if(state != 0)
  {
    free(state->pids);
    free(state);
  }
  MemoryZeroStruct(iter);
}
