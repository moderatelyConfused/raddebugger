// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#ifndef MAC_DEMON_H
#define MAC_DEMON_H

////////////////////////////////
//~ rjf: macOS Includes
//
// NOTE: the demon layer uses only libSystem / Mach headers (no Apple
// framework headers), so it can be syntax-checked on non-Apple hosts with
// Apple's open-source headers.

// NOTE: some MIG-generated Mach headers use `internal` as an identifier.
#undef internal
#include <mach/mach.h>
#include <mach/mach_types.h>
#include <mach/mach_init.h>
#include <mach/mach_port.h>
#include <mach/mach_error.h>
#include <mach/message.h>
#include <mach/exception_types.h>
#include <mach/task.h>
#include <mach/task_info.h>
#include <mach/thread_act.h>
#include <mach/thread_info.h>
#include <mach/thread_status.h>
#include <mach/vm_map.h>
#include <mach/vm_region.h>
#include <mach/vm_prot.h>
#include <mach/ndr.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <mach-o/dyld_images.h>
#include <sys/event.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/sysctl.h>
#include <sys/proc_info.h>
#include <libproc.h>
#include <spawn.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#define internal static

#if __has_include(<mach/mach_vm.h>)
# include <mach/mach_vm.h>
#else
// NOTE: MIG-generated prototypes from <mach/mach_vm.h>, exported by libSystem.
extern kern_return_t mach_vm_read_overwrite(vm_map_t target_task, mach_vm_address_t address, mach_vm_size_t size, mach_vm_address_t data, mach_vm_size_t *outsize);
extern kern_return_t mach_vm_write(vm_map_t target_task, mach_vm_address_t address, vm_offset_t data, mach_msg_type_number_t dataCnt);
extern kern_return_t mach_vm_protect(vm_map_t target_task, mach_vm_address_t address, mach_vm_size_t size, boolean_t set_maximum, vm_prot_t new_protection);
extern kern_return_t mach_vm_region(vm_map_t target_task, mach_vm_address_t *address, mach_vm_size_t *size, vm_region_flavor_t flavor, vm_region_info_t info, mach_msg_type_number_t *infoCnt, mach_port_t *object_name);
extern kern_return_t mach_vm_region_recurse(vm_map_t target_task, mach_vm_address_t *address, mach_vm_size_t *size, natural_t *nesting_depth, vm_region_recurse_info_t info, mach_msg_type_number_t *infoCnt);
extern kern_return_t mach_vm_allocate(vm_map_t target, mach_vm_address_t *address, mach_vm_size_t size, int flags);
extern kern_return_t mach_vm_deallocate(vm_map_t target, mach_vm_address_t address, mach_vm_size_t size);
#endif

#if __has_include(<sys/ptrace.h>)
# include <sys/ptrace.h>
#else
// NOTE: Darwin <sys/ptrace.h>
# define PT_TRACE_ME   0
# define PT_CONTINUE   7
# define PT_KILL       8
# define PT_ATTACH     10
# define PT_DETACH     11
# define PT_SIGEXC     12
# define PT_THUPDATE   13
# define PT_ATTACHEXC  14
extern int ptrace(int request, pid_t pid, caddr_t addr, int data);
#endif

#if !defined(PT_ATTACHEXC)
# define PT_ATTACHEXC 14
#endif
#if !defined(PT_THUPDATE)
# define PT_THUPDATE 13
#endif

#if !defined(_POSIX_SPAWN_DISABLE_ASLR)
# define _POSIX_SPAWN_DISABLE_ASLR 0x0100
#endif

extern kern_return_t task_for_pid(mach_port_name_t target_tport, int pid, mach_port_name_t *t);

////////////////////////////////
//~ rjf: Mach Exception Message Layouts
//
// Equivalent to the MIG-generated __Request__mach_exception_raise_t /
// __Reply__mach_exception_raise_t (MACH_EXCEPTION_CODES | EXCEPTION_DEFAULT).

#define MAC_DMN_MSG_ID_EXCEPTION_RAISE       2405
#define MAC_DMN_MSG_ID_EXCEPTION_RAISE_REPLY 2505

#pragma pack(push, 4)
typedef struct MAC_DMN_ExceptionRaiseRequest MAC_DMN_ExceptionRaiseRequest;
struct MAC_DMN_ExceptionRaiseRequest
{
  mach_msg_header_t head;
  mach_msg_body_t body;
  mach_msg_port_descriptor_t thread;
  mach_msg_port_descriptor_t task;
  NDR_record_t ndr;
  exception_type_t exception;
  mach_msg_type_number_t code_count;
  int64_t code[2];
};

typedef struct MAC_DMN_ExceptionRaiseReply MAC_DMN_ExceptionRaiseReply;
struct MAC_DMN_ExceptionRaiseReply
{
  mach_msg_header_t head;
  NDR_record_t ndr;
  kern_return_t ret_code;
};
#pragma pack(pop)

typedef union MAC_DMN_ExceptionMsgBuffer MAC_DMN_ExceptionMsgBuffer;
union MAC_DMN_ExceptionMsgBuffer
{
  MAC_DMN_ExceptionRaiseRequest request;
  mach_msg_header_t head;
  U8 bytes[1024];
};

////////////////////////////////
//~ rjf: Exception Code Encoding
//
// DMN_Event.code for exceptions reported from this layer: BSD signal numbers
// for soft-signal exceptions, 0x10000 + exception type for Mach exceptions
// (see MAC_ExceptionCodeTable in src/mac/mac.mdesk).

#define MAC_DMN_EXC_CODE_BASE 0x10000

////////////////////////////////
//~ rjf: Entities

typedef enum MAC_DMN_EntityKind
{
  MAC_DMN_EntityKind_Null,
  MAC_DMN_EntityKind_Process,
  MAC_DMN_EntityKind_ProcessCtx,
  MAC_DMN_EntityKind_Thread,
  MAC_DMN_EntityKind_Module,
}
MAC_DMN_EntityKind;

typedef enum MAC_DMN_ThreadState
{
  MAC_DMN_ThreadState_Null,
  MAC_DMN_ThreadState_Running,
  MAC_DMN_ThreadState_Stopped,
  MAC_DMN_ThreadState_Exited,
}
MAC_DMN_ThreadState;

typedef enum MAC_DMN_StopKind
{
  MAC_DMN_StopKind_Null,
  MAC_DMN_StopKind_Suspended,      // stopped only because the task was suspended
  MAC_DMN_StopKind_SoftSignal,     // EXC_SOFTWARE / EXC_SOFT_SIGNAL (pending reply)
  MAC_DMN_StopKind_HardException,  // EXC_BAD_ACCESS etc. (pending reply)
  MAC_DMN_StopKind_Breakpoint,     // brk hit (pending reply)
  MAC_DMN_StopKind_SingleStep,     // software step completion (pending reply)
  MAC_DMN_StopKind_Watchpoint,     // hardware watchpoint (pending reply)
}
MAC_DMN_StopKind;

typedef struct MAC_DMN_Thread MAC_DMN_Thread;
struct MAC_DMN_Thread
{
  MAC_DMN_Thread *next;
  MAC_DMN_Thread *prev;
  MAC_DMN_Thread *port_next;
  MAC_DMN_Thread *port_prev;
  struct MAC_DMN_Process *process;
  mach_port_t port;             // thread port (one send right owned by us)
  U64 tid;                      // system-wide unique thread id
  U64 tsd_base_vaddr;           // thread_identifier_info.thread_handle (TPIDRRO_EL0)
  MAC_DMN_ThreadState state;
  void *reg_block;
  B32 is_reg_block_dirty;       // the engine modified the block; write it back before resuming
  B32 regs_are_valid;           // the block holds real register values (a read succeeded, or the engine wrote it)
  B32 is_reported;              // CreateThread has been reported to the engine
  B32 is_suspended_by_us;       // thread_suspend() was called by this layer and not yet balanced
  B32 is_single_stepping;       // MDSCR_EL1.SS is set on this thread
  B32 is_seen;                  // scratch mark used when diffing task_threads()
  B32 stop_reported;            // this stop's events have been generated already
  B32 force_ignore_on_reply;    // the pending exception must be suppressed when replied (internal signals)
  
  // rjf: pending exception reply
  B32 has_pending_exception;
  MAC_DMN_ExceptionRaiseRequest pending_exception;
  MAC_DMN_StopKind stop_kind;
  U64 stop_signo;               // for soft signals
  U64 stop_hw_trap_vaddr;       // for watchpoints
  
  // rjf: exception repeat tracking
  U64 last_exception_ip;
  U64 last_exception_code;
  B32 escalated_exception;      // last reply escalated a hardware exception to a signal (KERN_FAILURE)
  U64 escalated_ip;
  
  // rjf: thread names
  U64 last_name_hash;
  U64 name_gather_time_us;
};

typedef struct MAC_DMN_ThreadSlot MAC_DMN_ThreadSlot;
struct MAC_DMN_ThreadSlot
{
  MAC_DMN_Thread *first;
  MAC_DMN_Thread *last;
};

typedef struct MAC_DMN_Module MAC_DMN_Module;
struct MAC_DMN_Module
{
  MAC_DMN_Module *order_next;
  MAC_DMN_Module *order_prev;
  MAC_DMN_Module *hash_next;
  MAC_DMN_Module *hash_prev;
  U64 base_vaddr;               // address of the mach_header_64
  U64 size;
  String8 path;
  U64 thread_vars_vaddr;        // __DATA,__thread_vars section (0 if none)
  U64 tls_key;                  // pthread key used by this image's TLVs (0 = unknown yet)
  B8 is_live;
  B8 is_main;
  B8 is_dyld;
};

typedef struct MAC_DMN_ModuleSlot MAC_DMN_ModuleSlot;
struct MAC_DMN_ModuleSlot
{
  MAC_DMN_Module *first;
  MAC_DMN_Module *last;
};

typedef enum MAC_DMN_ProcessState
{
  MAC_DMN_ProcessState_Null,
  MAC_DMN_ProcessState_Launch,    // spawned suspended, not yet reported
  MAC_DMN_ProcessState_Attach,    // attached, waiting for initial SIGSTOP soft signal
  MAC_DMN_ProcessState_Normal,
}
MAC_DMN_ProcessState;

typedef struct MAC_DMN_Process MAC_DMN_Process;
struct MAC_DMN_Process
{
  MAC_DMN_Process *next;
  MAC_DMN_Process *prev;
  MAC_DMN_Process *pid_next;
  MAC_DMN_Process *pid_prev;
  struct MAC_DMN_ProcessCtx *ctx;
  pid_t pid;
  mach_port_t task;
  MAC_DMN_ProcessState state;
  B32 debug_subprocesses;
  B32 is_child;                 // launched by us (must be reaped via waitpid)
  B32 has_exited;
  B32 needs_initial_continue;   // spawned start-suspended; first resume must also continue the BSD process
  U64 exit_code;
  U64 task_suspend_count;       // number of task_suspend() calls we must balance
  U64 thread_count;
  MAC_DMN_Thread *first_thread;
  MAC_DMN_Thread *last_thread;
  
  // rjf: original exception ports (restored on detach)
  exception_mask_t saved_exc_masks[EXC_TYPES_COUNT];
  mach_port_t saved_exc_ports[EXC_TYPES_COUNT];
  exception_behavior_t saved_exc_behaviors[EXC_TYPES_COUNT];
  thread_state_flavor_t saved_exc_flavors[EXC_TYPES_COUNT];
  mach_msg_type_number_t saved_exc_count;
  
  // rjf: dyld
  U64 all_image_infos_vaddr;    // task_info(TASK_DYLD_INFO)
  U64 dyld_base_vaddr;
  U64 dyld_notification_vaddr;  // address of _dyld_debugger_notification (0 if unknown)
  B32 dyld_notification_trap_is_set;
  U32 dyld_notification_orig_inst;
  U32 last_image_info_count;
};

typedef struct MAC_DMN_ProcessSlot MAC_DMN_ProcessSlot;
struct MAC_DMN_ProcessSlot
{
  MAC_DMN_Process *first;
  MAC_DMN_Process *last;
};

typedef struct MAC_DMN_ProcessCtx MAC_DMN_ProcessCtx;
struct MAC_DMN_ProcessCtx
{
  Arena *arena;
  Arch arch;
  MAC_DMN_Module *first_module;
  MAC_DMN_Module *last_module;
  U64 module_count;
  U64 module_slots_count;
  MAC_DMN_ModuleSlot *module_slots;
  U64 ref_count;
  String8List free_reg_blocks;
  String8List free_reg_block_nodes;
};

typedef struct MAC_DMN_Entity MAC_DMN_Entity;
struct MAC_DMN_Entity
{
  union
  {
    MAC_DMN_Process process;
    MAC_DMN_ProcessCtx process_ctx;
    MAC_DMN_Thread thread;
    MAC_DMN_Module module;
    MAC_DMN_Entity *next;
  };
  U32 gen;
  MAC_DMN_EntityKind kind;
};

////////////////////////////////
//~ rjf: In-Memory Image Info

typedef struct MAC_DMN_ImageInfo MAC_DMN_ImageInfo;
struct MAC_DMN_ImageInfo
{
  B32 valid;
  U32 cputype;
  U32 filetype;
  U64 header_vaddr;
  U64 slide;                    // header_vaddr - __TEXT vmaddr
  U64 vsize;                    // extent of all segments
  U64 entry_point_voff;
  Guid uuid;
  Rng1U64 text_vaddr_range;
  Rng1U64 unwind_info_vaddr_range;
  Rng1U64 eh_frame_vaddr_range;
  U64 thread_vars_vaddr;
  // rjf: symbol table (in-memory addresses)
  U64 symtab_vaddr;
  U64 symtab_count;
  U64 strtab_vaddr;
  U64 strtab_size;
};

////////////////////////////////
//~ rjf: Top-Level State Bundle

typedef struct MAC_DMN_State MAC_DMN_State;
struct MAC_DMN_State
{
  Arena *arena;
  
  // rjf: access locking mechanism
  Mutex access_mutex;
  B32 access_run_state;
  
  // rjf: main entity storage
  Arena *entities_arena;
  MAC_DMN_Entity *entities_base;
  MAC_DMN_Entity *free_entity;
  U64 entities_count;
  
  // rjf: id -> entity tables
  MAC_DMN_ProcessSlot *process_from_pid_slots;
  U64 process_from_pid_slots_count;
  MAC_DMN_ThreadSlot *thread_from_port_slots;
  U64 thread_from_port_slots_count;
  
  // rjf: process entity list
  MAC_DMN_Process *first_process;
  MAC_DMN_Process *last_process;
  U64 process_count;
  U64 process_pending_creation;
  
  // rjf: mach exception port / kqueue
  mach_port_t exception_port;
  mach_port_t port_set;
  int kq;
  
  // rjf: halting state
  Mutex halter_mutex;
  B32 halt_requested;
  U64 halt_code;
  U64 halt_user_data;
  
  // rjf: page size
  U64 page_size;
};

////////////////////////////////
//~ rjf: Globals

global MAC_DMN_State *mac_dmn_state = 0;
thread_static B32 mac_dmn_ctrl_thread = 0;

////////////////////////////////
//~ rjf: Memory R/W Helpers

internal U64 mac_dmn_read(mach_port_t task, Rng1U64 range, void *dst);
internal B32 mac_dmn_write(mach_port_t task, Rng1U64 range, void *src);
internal String8 mac_dmn_read_string_capped(Arena *arena, mach_port_t task, U64 base_vaddr, U64 cap_size);
#define mac_dmn_read_struct(task, vaddr, ptr)  mac_dmn_read((task), r1u64((vaddr), (vaddr)+sizeof(*(ptr))), (ptr))
#define mac_dmn_write_struct(task, vaddr, ptr) mac_dmn_write((task), r1u64((vaddr), (vaddr)+sizeof(*(ptr))), (ptr))

////////////////////////////////
//~ rjf: In-Memory Mach-O Parsing Helpers

internal MAC_DMN_ImageInfo mac_dmn_image_info_from_header_vaddr(mach_port_t task, U64 header_vaddr);
internal U64 mac_dmn_symbol_vaddr_from_image(mach_port_t task, MAC_DMN_ImageInfo *image, String8 name);
internal U64 mac_dmn_image_header_vaddr_from_vaddr_scan(mach_port_t task, U64 vaddr, U32 filetype);

////////////////////////////////
//~ rjf: Entity Functions

internal MAC_DMN_Entity *mac_dmn_entity_alloc(MAC_DMN_EntityKind kind);
internal void mac_dmn_entity_release(MAC_DMN_Entity *entity);
internal MAC_DMN_Process *mac_dmn_process_alloc(pid_t pid, mach_port_t task, MAC_DMN_ProcessState state, B32 debug_subprocesses, B32 is_child);
internal void mac_dmn_process_release(MAC_DMN_Process *process);
internal MAC_DMN_Thread *mac_dmn_thread_alloc(MAC_DMN_Process *process, mach_port_t port);
internal void mac_dmn_thread_release(MAC_DMN_Thread *thread);
internal MAC_DMN_Module *mac_dmn_module_from_base_vaddr(MAC_DMN_ProcessCtx *ctx, U64 base_vaddr);
internal MAC_DMN_Module *mac_dmn_module_alloc(MAC_DMN_ProcessCtx *ctx, U64 base_vaddr, U64 size, String8 path, U64 thread_vars_vaddr, B32 is_main, B32 is_dyld);
internal void mac_dmn_module_release(MAC_DMN_ProcessCtx *ctx, MAC_DMN_Module *module);
internal DMN_Handle mac_dmn_handle_from_entity(MAC_DMN_Entity *entity);
internal DMN_Handle mac_dmn_handle_from_process(MAC_DMN_Process *process);
internal DMN_Handle mac_dmn_handle_from_thread(MAC_DMN_Thread *thread);
internal DMN_Handle mac_dmn_handle_from_module(MAC_DMN_Module *module);
internal MAC_DMN_Entity *mac_dmn_entity_from_handle(DMN_Handle handle, MAC_DMN_EntityKind expected_kind);
internal MAC_DMN_Process *mac_dmn_process_from_handle(DMN_Handle handle);
internal MAC_DMN_Thread *mac_dmn_thread_from_handle(DMN_Handle handle);
internal MAC_DMN_Module *mac_dmn_module_from_handle(DMN_Handle handle);
internal MAC_DMN_Process *mac_dmn_process_from_pid(pid_t pid);
internal MAC_DMN_Process *mac_dmn_process_from_task(mach_port_t task);
internal MAC_DMN_Thread *mac_dmn_thread_from_port(mach_port_t port);

////////////////////////////////
//~ rjf: Thread Helpers

internal void mac_dmn_thread_refresh_identifier_info(MAC_DMN_Thread *thread);
internal U64  mac_dmn_ip_from_thread(MAC_DMN_Thread *thread);
internal void mac_dmn_thread_write_ip(MAC_DMN_Thread *thread, U64 ip);
internal B32  mac_dmn_thread_read_reg_block(MAC_DMN_Thread *thread);
internal B32  mac_dmn_thread_write_reg_block(MAC_DMN_Thread *thread);
internal B32  mac_dmn_thread_set_single_step(MAC_DMN_Thread *thread, B32 is_on);
internal void mac_dmn_thread_reply_pending_exception(MAC_DMN_Thread *thread, B32 ignore_exception);
internal void mac_dmn_process_sync_threads(Arena *arena, DMN_EventList *events, MAC_DMN_Process *process, B32 report);
internal void mac_dmn_process_suspend(MAC_DMN_Process *process);
internal void mac_dmn_process_resume(MAC_DMN_Process *process);
internal B32  mac_dmn_process_receive_exceptions(MAC_DMN_Process *process, U64 timeout_ms);

////////////////////////////////
//~ rjf: Module / dyld Helpers

internal DMN_ModuleInfo *mac_dmn_module_info_from_image(Arena *arena, MAC_DMN_Process *process, MAC_DMN_ImageInfo *image, String8 path);
internal void mac_dmn_push_event_load_module(Arena *arena, DMN_EventList *events, MAC_DMN_Process *process, MAC_DMN_Thread *thread, U64 header_vaddr, String8 path, B32 is_main, B32 is_dyld);
internal void mac_dmn_push_event_unload_module(Arena *arena, DMN_EventList *events, MAC_DMN_Process *process, MAC_DMN_Module *module);
internal void mac_dmn_process_sync_images(Arena *arena, DMN_EventList *events, MAC_DMN_Process *process, MAC_DMN_Thread *thread);
internal void mac_dmn_process_set_dyld_notification_trap(MAC_DMN_Process *process, B32 is_set);
internal U64  mac_dmn_strip_pac(U64 ptr);
internal B32  mac_dmn_process_find_dyld_notification(MAC_DMN_Process *process, U64 dyld_header_vaddr, B32 dyld_is_rebased);
internal void mac_dmn_process_check_images(Arena *arena, DMN_EventList *events, MAC_DMN_Process *process);
internal B32  mac_dmn_process_init_ctx(Arena *arena, DMN_EventList *events, MAC_DMN_Process *process, B32 is_launch);

#endif // MAC_DEMON_H
