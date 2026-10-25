// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#ifndef MAC_BASE_H
#define MAC_BASE_H

////////////////////////////////
//~ rjf: Includes

// NOTE(rjf): the MIG-generated Mach headers (mach/mach_host.h, mach/task_info.h)
// use `internal` as an identifier, which the base layer #defines to `static`;
// so the keyword macro is removed around them & restored afterwards.
#undef internal
#include <mach/mach.h>
#include <mach/mach_init.h>
#include <mach/mach_host.h>
#include <mach/mach_time.h>
#include <mach/host_info.h>
#include <mach/vm_statistics.h>
#include <mach/semaphore.h>
#include <mach/task.h>
#include <mach/clock_types.h>
#include <mach/sync_policy.h>
#define internal static

#include <copyfile.h>
#include <crt_externs.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <poll.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/proc.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/syslimits.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef struct tm tm;
typedef struct timespec timespec;

////////////////////////////////
//~ rjf: macOS Call Interruption Retry Helper

#define MAC_RETRY_ON_EINTR(expr)             \
(__extension__({                           \
__typeof__(expr) __ret;                    \
do {                                       \
__ret = (expr);                          \
} while ((__ret == -1) && errno == EINTR); \
__ret;                                     \
}))

////////////////////////////////
//~ rjf: File Iterator

typedef struct MAC_FileIter MAC_FileIter;
struct MAC_FileIter
{
  DIR *dir;
  struct dirent *dp;
  String8 path;
};
StaticAssert(sizeof(Member(FileIter, memory)) >= sizeof(MAC_FileIter), mac_file_iter_size_check);

////////////////////////////////
//~ rjf: Safe Call Handler Chain

typedef struct MAC_SafeCallChain MAC_SafeCallChain;
struct MAC_SafeCallChain
{
  MAC_SafeCallChain *next;
  ThreadEntryPointFunctionType *fail_handler;
  void *ptr;
};

////////////////////////////////
//~ rjf: Entities

typedef enum MAC_EntityKind
{
  MAC_EntityKind_Thread,
  MAC_EntityKind_Mutex,
  MAC_EntityKind_RWMutex,
  MAC_EntityKind_ConditionVariable,
  MAC_EntityKind_Barrier,
  MAC_EntityKind_Semaphore,
}
MAC_EntityKind;

// NOTE(rjf): macOS limits POSIX semaphore / shared memory names to 31
// characters (PSEMNAMLEN / PSHMNAMLEN), so every name the callers pass is
// mapped to "/rad_<16 hex digits>" (see mac_ipc_name_from_name); that is the
// largest name we ever store.
#define MAC_IPC_NAME_CAP 32

typedef struct MAC_Entity MAC_Entity;
struct MAC_Entity
{
  MAC_Entity *next;
  MAC_EntityKind kind;
  union
  {
    struct
    {
      pthread_t handle;
      ThreadEntryPointFunctionType *func;
      void *ptr;
    } thread;
    pthread_mutex_t mutex_handle;
    pthread_rwlock_t rwmutex_handle;
    struct
    {
      pthread_cond_t cond_handle;
      pthread_mutex_t rwlock_mutex_handle;
    } cv;
    struct
    {
      // NOTE(rjf): macOS has no pthread_barrier_t; barriers are built from a
      // mutex + condition variable + generation counter.
      pthread_mutex_t mutex;
      pthread_cond_t cond;
      U64 count;
      U64 waiting;
      U64 generation;
    } barrier;
    struct
    {
      // NOTE(rjf): macOS has no sem_init/sem_timedwait; unnamed semaphores
      // are Mach semaphores (task-local), named ones (cross-process) are
      // POSIX named semaphores.
      B32 is_named;
      B32 is_owner;
      semaphore_t mach_handle;
      sem_t *posix_handle;
      char name[MAC_IPC_NAME_CAP];
    } semaphore;
  };
};

////////////////////////////////
//~ rjf: Process Groups

typedef struct MAC_ProcessGroup MAC_ProcessGroup;
struct MAC_ProcessGroup
{
  pid_t pid;
  B32 kill_on_close;
};

////////////////////////////////
//~ rjf: State

typedef struct MAC_State MAC_State;
struct MAC_State
{
  Arena *arena;
  SystemInfo system_info;
  ProcessInfo process_info;
  pthread_mutex_t entity_mutex;
  Arena *entity_arena;
  MAC_Entity *entity_free;
  U64 default_env_count;
  char **default_env;
  U32 unnamed_shared_memory_counter;
};

////////////////////////////////
//~ rjf: Globals

global MAC_State mac_state = {0};
thread_static MAC_SafeCallChain *mac_safe_call_chain = 0;

////////////////////////////////
//~ rjf: Helpers

internal DateTime mac_date_time_from_tm(tm in, U32 msec);
internal tm mac_tm_from_date_time(DateTime dt);
internal timespec mac_timespec_from_date_time(DateTime dt);
internal DenseTime mac_dense_time_from_timespec(timespec in);
internal FileProperties mac_file_properties_from_stat(struct stat *s);
internal void mac_safe_call_sig_handler(int sig, siginfo_t *info, void *context);
internal String8 mac_ipc_name_from_name(Arena *arena, String8 name);
internal U64 mac_relative_us_from_endt_us(U64 endt_us);
internal timespec mac_relative_timespec_from_endt_us(U64 endt_us);
internal B32 mac_kinfo_proc_from_pid(pid_t pid, struct kinfo_proc *out);

////////////////////////////////
//~ rjf: Entities

internal MAC_Entity *mac_entity_alloc(MAC_EntityKind kind);
internal void mac_entity_release(MAC_Entity *entity);

////////////////////////////////
//~ rjf: Thread Entry Point

internal void *mac_thread_entry_point(void *ptr);

#endif // MAC_BASE_H
