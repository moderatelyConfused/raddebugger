// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

////////////////////////////////
//~ rjf: Helpers

internal DateTime
mac_date_time_from_tm(tm in, U32 msec)
{
  DateTime dt = {0};
  dt.sec  = in.tm_sec;
  dt.min  = in.tm_min;
  dt.hour = in.tm_hour;
  dt.day  = in.tm_mday-1;
  dt.mon  = in.tm_mon;
  dt.year = in.tm_year+1900;
  dt.msec = msec;
  return dt;
}

internal tm
mac_tm_from_date_time(DateTime dt)
{
  tm result = {0};
  result.tm_sec = dt.sec;
  result.tm_min = dt.min;
  result.tm_hour= dt.hour;
  result.tm_mday= dt.day+1;
  result.tm_mon = dt.mon;
  result.tm_year= dt.year-1900;
  return result;
}

internal timespec
mac_timespec_from_date_time(DateTime dt)
{
  tm tm_val = mac_tm_from_date_time(dt);
  time_t seconds = timegm(&tm_val);
  timespec result = {0};
  result.tv_sec = seconds;
  return result;
}

internal DenseTime
mac_dense_time_from_timespec(timespec in)
{
  DenseTime result = 0;
  {
    struct tm tm_time = {0};
    gmtime_r(&in.tv_sec, &tm_time);
    DateTime date_time = mac_date_time_from_tm(tm_time, in.tv_nsec/Million(1));
    result = dense_time_from_date_time(date_time);
  }
  return result;
}

internal FileProperties
mac_file_properties_from_stat(struct stat *s)
{
  FileProperties props = {0};
  props.size     = s->st_size;
  props.created  = mac_dense_time_from_timespec(s->st_birthtimespec);
  props.modified = mac_dense_time_from_timespec(s->st_mtimespec);
  if(S_ISDIR(s->st_mode))
  {
    props.flags |= FilePropertyFlag_IsFolder;
  }
  return props;
}

internal void
mac_safe_call_sig_handler(int sig, siginfo_t *info, void *context)
{
  MAC_SafeCallChain *chain = mac_safe_call_chain;
  if(chain != 0 && chain->fail_handler != 0)
  {
    chain->fail_handler(chain->ptr);
  }
  abort();
}

internal String8
mac_ipc_name_from_name(Arena *arena, String8 name)
{
  // NOTE(rjf): POSIX semaphore & shared memory names on macOS may not exceed 31
  // characters (PSEMNAMLEN / PSHMNAMLEN), but the callers pass descriptive names
  // (e.g. "conversion_completion_shared_memory_pid_12345"), so every name is
  // deterministically mapped to "/rad_<16 hex digits of its 64-bit hash>". The
  // same mapping is used by the alloc & open paths, so all processes agree.
  U64 hash = u64_hash_from_str8(name);
  char *hex_digits = "0123456789abcdef";
  U8 buffer[5+16] = {'/','r','a','d','_'};
  for EachIndex(idx, 16)
  {
    buffer[5+idx] = hex_digits[(hash >> (60 - idx*4)) & 0xf];
  }
  String8 result = str8_copy(arena, str8(buffer, sizeof(buffer)));
  return result;
}

internal U64
mac_relative_us_from_endt_us(U64 endt_us)
{
  U64 now_us = now_time_us();
  U64 result = (endt_us > now_us) ? (endt_us - now_us) : 0;
  return result;
}

internal timespec
mac_relative_timespec_from_endt_us(U64 endt_us)
{
  // NOTE(rjf): macOS has no absolute-monotonic-deadline waits, so deadlines are
  // converted to relative timeouts. The kernel turns those into 64-bit
  // nanosecond counts, so the seconds are clamped to keep absurdly large (but
  // not max_U64) deadlines from overflowing into an immediate timeout.
  U64 rel_us = mac_relative_us_from_endt_us(endt_us);
  U64 rel_sec = rel_us/Million(1);
  if(rel_sec > 0x7fffffff) { rel_sec = 0x7fffffff; }
  timespec result = {0};
  result.tv_sec  = (time_t)rel_sec;
  result.tv_nsec = (long)(Thousand(1) * (rel_us - (rel_us/Million(1))*Million(1)));
  return result;
}

internal B32
mac_kinfo_proc_from_pid(pid_t pid, struct kinfo_proc *out)
{
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, (int)pid};
  size_t size = sizeof(*out);
  MemoryZeroStruct(out);
  int err = sysctl(mib, ArrayCount(mib), out, &size, 0, 0);
  B32 result = (err == 0 && size != 0);
  return result;
}

////////////////////////////////
//~ rjf: Entities

internal MAC_Entity *
mac_entity_alloc(MAC_EntityKind kind)
{
  MAC_Entity *entity = 0;
  DeferLoop(pthread_mutex_lock(&mac_state.entity_mutex),
            pthread_mutex_unlock(&mac_state.entity_mutex))
  {
    entity = mac_state.entity_free;
    if(entity)
    {
      SLLStackPop(mac_state.entity_free);
    }
    else
    {
      entity = push_array_no_zero(mac_state.entity_arena, MAC_Entity, 1);
    }
  }
  MemoryZeroStruct(entity);
  entity->kind = kind;
  return entity;
}

internal void
mac_entity_release(MAC_Entity *entity)
{
  DeferLoop(pthread_mutex_lock(&mac_state.entity_mutex),
            pthread_mutex_unlock(&mac_state.entity_mutex))
  {
    SLLStackPush(mac_state.entity_free, entity);
  }
}

////////////////////////////////
//~ rjf: Thread Entry Point

internal void *
mac_thread_entry_point(void *ptr)
{
  MAC_Entity *entity = (MAC_Entity *)ptr;
  ThreadEntryPointFunctionType *func = entity->thread.func;
  void *thread_ptr = entity->thread.ptr;
  supplement_thread_base_entry_point(func, thread_ptr);
  return 0;
}

////////////////////////////////
//~ rjf: @per_os_impl Debugger Attachment Checking

internal B32
debugger_is_attached(void)
{
  B32 result = 0;
  struct kinfo_proc info = {0};
  if(mac_kinfo_proc_from_pid(getpid(), &info))
  {
    result = ((info.kp_proc.p_flag & P_TRACED) != 0);
  }
  return result;
}

////////////////////////////////
//~ rjf: @per_os_impl Platform Time Functions

internal U64
now_time_us(void)
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  U64 result = (U64)t.tv_sec*Million(1) + ((U64)t.tv_nsec/Thousand(1));
  return result;
}

internal U32
now_time_unix(void)
{
  time_t t = time(0);
  return (U32)t;
}

internal DateTime
now_time_universal(void)
{
  time_t t = 0;
  time(&t);
  struct tm universal_tm = {0};
  gmtime_r(&t, &universal_tm);
  DateTime result = mac_date_time_from_tm(universal_tm, 0);
  return result;
}

internal DateTime
universal_from_local_time(DateTime *dt)
{
  // rjf: local DateTime -> universal time_t
  tm local_tm = mac_tm_from_date_time(*dt);
  local_tm.tm_isdst = -1;
  time_t universal_t = mktime(&local_tm);

  // rjf: universal time_t -> DateTime
  tm universal_tm = {0};
  gmtime_r(&universal_t, &universal_tm);
  DateTime result = mac_date_time_from_tm(universal_tm, 0);
  return result;
}

internal DateTime
local_from_universal_time(DateTime *dt)
{
  // rjf: universal DateTime -> local time_t
  tm universal_tm = mac_tm_from_date_time(*dt);
  universal_tm.tm_isdst = -1;
  time_t universal_t = timegm(&universal_tm);
  tm local_tm = {0};
  localtime_r(&universal_t, &local_tm);

  // rjf: local tm -> DateTime
  DateTime result = mac_date_time_from_tm(local_tm, 0);
  return result;
}

internal void
sleep_ms(U32 ms)
{
  usleep((useconds_t)ms*Thousand(1));
}

////////////////////////////////
//~ rjf: @per_os_impl Platform GUID Functions

internal Guid
make_guid(void)
{
  Guid guid = {0};
  arc4random_buf(guid.v, sizeof(guid.v));
  guid.data3 &= 0x0fff;
  guid.data3 |= (4 << 12);
  guid.data4[0] &= 0x3f;
  guid.data4[0] |= 0x80;
  return guid;
}

////////////////////////////////
//~ rjf: @per_os_impl Platform Memory Allocation

//- rjf: basic

internal void *
reserve_memory(U64 size)
{
  void *result = mmap(0, size, PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
  if(result == MAP_FAILED)
  {
    result = 0;
  }
  return result;
}

internal B32
commit_memory(void *ptr, U64 size)
{
  return mprotect(ptr, size, PROT_READ|PROT_WRITE) == 0;
}

internal void
decommit_memory(void *ptr, U64 size)
{
  madvise(ptr, size, MADV_FREE);
  mprotect(ptr, size, PROT_NONE);
}

internal void
release_memory(void *ptr, U64 size)
{
  munmap(ptr, size);
}

//- rjf: large pages

internal void *
reserve_memory_large(U64 size)
{
  // NOTE(rjf): no superpage support on Apple Silicon; plain reservation.
  void *result = mmap(0, size, PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
  if(result == MAP_FAILED)
  {
    result = 0;
  }
  return result;
}

internal B32
commit_memory_large(void *ptr, U64 size)
{
  mprotect(ptr, size, PROT_READ|PROT_WRITE);
  return 1;
}

////////////////////////////////
//~ rjf: @per_os_impl Shared Memory

internal SharedMemory
shared_memory_alloc(U64 size, String8 name)
{
  SharedMemory result = { max_U64 };

  if(size == 0 || size > max_S64) { return result; }

  Temp scratch = scratch_begin(0, 0);
  int id = -1;
  if(name.size == 0)
  {
    // rjf: unnamed -> create a uniquely-named object & unlink it right away;
    // the descriptor stays valid until closed.
    for(U32 attempt_idx = 0; attempt_idx < 64; attempt_idx += 1)
    {
      U32 counter = ins_atomic_u32_inc_eval(&mac_state.unnamed_shared_memory_counter);
      String8 unique_name = str8f(scratch.arena, "/radu_%x_%x", (U32)getpid(), counter);
      id = shm_open((char *)unique_name.str, O_RDWR|O_CREAT|O_EXCL, 0600);
      if(id >= 0)
      {
        shm_unlink((char *)unique_name.str);
        break;
      }
      if(errno != EEXIST)
      {
        break;
      }
    }
  }
  else
  {
    // rjf: named -> remove any stale object left behind by a previous run (views
    // other processes still hold stay valid), then create a fresh one.
    String8 ipc_name = mac_ipc_name_from_name(scratch.arena, name);
    shm_unlink((char *)ipc_name.str);
    id = shm_open((char *)ipc_name.str, O_RDWR|O_CREAT|O_EXCL, 0600);
    if(id < 0)
    {
      id = shm_open((char *)ipc_name.str, O_RDWR|O_CREAT, 0600);
    }
  }

  if(id >= 0)
  {
    fcntl(id, F_SETFD, FD_CLOEXEC);
    B32 sized = (ftruncate(id, (off_t)size) == 0);
    if(!sized)
    {
      // NOTE(rjf): macOS only allows sizing a shared memory object once; accept
      // an already-large-enough object.
      struct stat st = {0};
      sized = (fstat(id, &st) == 0 && st.st_size >= (off_t)size);
    }
    if(sized) { result.u64[0] = (U64)id; }
    else      { close(id); }
  }

  scratch_end(scratch);
  return result;
}

internal SharedMemory
shared_memory_open(String8 name)
{
  Temp scratch = scratch_begin(0, 0);
  String8      ipc_name = mac_ipc_name_from_name(scratch.arena, name);
  int          id       = shm_open((char *)ipc_name.str, O_RDWR, 0);
  if(id >= 0)
  {
    fcntl(id, F_SETFD, FD_CLOEXEC);
  }
  SharedMemory result = {id >= 0 ? (U64)id : max_U64};
  scratch_end(scratch);
  return result;
}

internal void
shared_memory_close(SharedMemory handle)
{
  close((int)handle.u64[0]);
}

internal void *
shared_memory_view_open(SharedMemory handle, Rng1U64 range)
{
  int id = (int)handle.u64[0];
  void *base = mmap(0, dim_1u64(range), PROT_READ|PROT_WRITE, MAP_SHARED, id, range.min);
  if(base == MAP_FAILED)
  {
    base = 0;
  }
  return base;
}

internal void
shared_memory_view_close(SharedMemory handle, void *ptr, Rng1U64 range)
{
  munmap(ptr, dim_1u64(range));
}

////////////////////////////////
//~ rjf: @per_os_impl System Info

internal SystemInfo *
get_system_info(void)
{
  return &mac_state.system_info;
}

////////////////////////////////
//~ rjf: @per_os_impl Current Thread Info

internal U32
tid(void)
{
  U64 tid64 = 0;
  pthread_threadid_np(0, &tid64);
  U32 result = (U32)tid64;
  return result;
}

internal void
set_platform_thread_name(String8 name)
{
  // NOTE(rjf): macOS can only name the calling thread, and names are limited
  // to 63 characters.
  Temp scratch = scratch_begin(0, 0);
  String8 name_copy = str8_copy(scratch.arena, str8(name.str, Min(name.size, 63)));
  pthread_setname_np((char *)name_copy.str);
  scratch_end(scratch);
}

////////////////////////////////
//~ rjf: @per_os_impl Thread Functions

internal Thread
thread_launch(ThreadEntryPointFunctionType *f, void *p)
{
  ProfBeginFunction();
  MAC_Entity *entity = mac_entity_alloc(MAC_EntityKind_Thread);
  entity->thread.func = f;
  entity->thread.ptr = p;
  {
    int pthread_result = pthread_create(&entity->thread.handle, 0, mac_thread_entry_point, entity);
    if(pthread_result != 0)
    {
      mac_entity_release(entity);
      entity = 0;
    }
  }
  Thread handle = {(U64)entity};
  ProfEnd();
  return handle;
}

internal B32
thread_join(Thread thread, U64 endt_us)
{
  if(MemoryIsZeroStruct(&thread)) { return 0; }
  MAC_Entity *entity = (MAC_Entity *)thread.u64[0];
  int join_result = pthread_join(entity->thread.handle, 0);
  B32 result = (join_result == 0);
  mac_entity_release(entity);
  return result;
}

internal void
thread_detach(Thread thread)
{
  if(MemoryIsZeroStruct(&thread)) { return; }
  MAC_Entity *entity = (MAC_Entity *)thread.u64[0];
  pthread_detach(entity->thread.handle);
  mac_entity_release(entity);
}

////////////////////////////////
//~ rjf: @per_os_impl Synchronization Primitive Functions

//- rjf: recursive mutexes

internal Mutex
mutex_alloc(void)
{
  MAC_Entity *entity = mac_entity_alloc(MAC_EntityKind_Mutex);
  int init_result = pthread_mutex_init(&entity->mutex_handle, 0);
  if(init_result != 0)
  {
    mac_entity_release(entity);
    entity = 0;
  }
  Mutex handle = {(U64)entity};
  return handle;
}

internal void
mutex_release(Mutex mutex)
{
  if(MemoryIsZeroStruct(&mutex)) { return; }
  MAC_Entity *entity = (MAC_Entity *)mutex.u64[0];
  pthread_mutex_destroy(&entity->mutex_handle);
  mac_entity_release(entity);
}

internal void
mutex_take(Mutex mutex)
{
  if(MemoryIsZeroStruct(&mutex)) { return; }
  MAC_Entity *entity = (MAC_Entity *)mutex.u64[0];
  pthread_mutex_lock(&entity->mutex_handle);
}

internal void
mutex_drop(Mutex mutex)
{
  if(MemoryIsZeroStruct(&mutex)) { return; }
  MAC_Entity *entity = (MAC_Entity *)mutex.u64[0];
  pthread_mutex_unlock(&entity->mutex_handle);
}

//- rjf: reader/writer mutexes

internal RWMutex
rw_mutex_alloc(void)
{
  MAC_Entity *entity = mac_entity_alloc(MAC_EntityKind_RWMutex);
  int init_result = pthread_rwlock_init(&entity->rwmutex_handle, 0);
  if(init_result != 0)
  {
    mac_entity_release(entity);
    entity = 0;
  }
  RWMutex handle = {(U64)entity};
  return handle;
}

internal void
rw_mutex_release(RWMutex mutex)
{
  if(MemoryIsZeroStruct(&mutex)) { return; }
  MAC_Entity *entity = (MAC_Entity *)mutex.u64[0];
  pthread_rwlock_destroy(&entity->rwmutex_handle);
  mac_entity_release(entity);
}

internal void
rw_mutex_take(RWMutex mutex, B32 write_mode)
{
  if(MemoryIsZeroStruct(&mutex)) { return; }
  MAC_Entity *entity = (MAC_Entity *)mutex.u64[0];
  if(write_mode)
  {
    pthread_rwlock_wrlock(&entity->rwmutex_handle);
  }
  else
  {
    pthread_rwlock_rdlock(&entity->rwmutex_handle);
  }
}

internal void
rw_mutex_drop(RWMutex mutex, B32 write_mode)
{
  if(MemoryIsZeroStruct(&mutex)) { return; }
  MAC_Entity *entity = (MAC_Entity *)mutex.u64[0];
  pthread_rwlock_unlock(&entity->rwmutex_handle);
}

//- rjf: condition variables

internal CondVar
cond_var_alloc(void)
{
  // NOTE(rjf): macOS has no pthread_condattr_setclock; waits are done with
  // relative timeouts (pthread_cond_timedwait_relative_np) computed from the
  // monotonic deadline, so the default clock is fine here.
  MAC_Entity *entity = mac_entity_alloc(MAC_EntityKind_ConditionVariable);
  int init_result = pthread_cond_init(&entity->cv.cond_handle, 0);
  if(init_result != 0)
  {
    mac_entity_release(entity);
    entity = 0;
  }
  int init2_result = 0;
  if(entity)
  {
    init2_result = pthread_mutex_init(&entity->cv.rwlock_mutex_handle, 0);
  }
  if(entity && init2_result != 0)
  {
    pthread_cond_destroy(&entity->cv.cond_handle);
    mac_entity_release(entity);
    entity = 0;
  }
  CondVar handle = {(U64)entity};
  return handle;
}

internal void
cond_var_release(CondVar cv)
{
  if(MemoryIsZeroStruct(&cv)) { return; }
  MAC_Entity *entity = (MAC_Entity *)cv.u64[0];
  pthread_cond_destroy(&entity->cv.cond_handle);
  pthread_mutex_destroy(&entity->cv.rwlock_mutex_handle);
  mac_entity_release(entity);
}

internal B32
cond_var_wait(CondVar cv, Mutex mutex, U64 endt_us)
{
  if(MemoryIsZeroStruct(&cv)) { return 0; }
  if(MemoryIsZeroStruct(&mutex)) { return 0; }
  MAC_Entity *cv_entity = (MAC_Entity *)cv.u64[0];
  MAC_Entity *mutex_entity = (MAC_Entity *)mutex.u64[0];
  int wait_result = 0;
  if(endt_us == max_U64)
  {
    wait_result = pthread_cond_wait(&cv_entity->cv.cond_handle, &mutex_entity->mutex_handle);
  }
  else
  {
    timespec rel_timespec = mac_relative_timespec_from_endt_us(endt_us);
    wait_result = pthread_cond_timedwait_relative_np(&cv_entity->cv.cond_handle, &mutex_entity->mutex_handle, &rel_timespec);
  }
  B32 result = (wait_result != ETIMEDOUT);
  return result;
}

internal B32
cond_var_wait_rw(CondVar cv, RWMutex mutex_rw, B32 write_mode, U64 endt_us)
{
  // TODO(rjf): because pthread does not supply cv/rw natively, I had to hack
  // this together, but this would probably just be a lot better if we just
  // implemented the primitives ourselves with e.g. futexes
  //
  if(MemoryIsZeroStruct(&cv)) { return 0; }
  if(MemoryIsZeroStruct(&mutex_rw)) { return 0; }
  MAC_Entity *cv_entity = (MAC_Entity *)cv.u64[0];
  MAC_Entity *rw_mutex_entity = (MAC_Entity *)mutex_rw.u64[0];
  B32 result = 0;
  pthread_mutex_lock(&cv_entity->cv.rwlock_mutex_handle);
  pthread_rwlock_unlock(&rw_mutex_entity->rwmutex_handle);
  {
    int wait_result = 0;
    if(endt_us == max_U64)
    {
      wait_result = pthread_cond_wait(&cv_entity->cv.cond_handle, &cv_entity->cv.rwlock_mutex_handle);
    }
    else
    {
      timespec rel_timespec = mac_relative_timespec_from_endt_us(endt_us);
      wait_result = pthread_cond_timedwait_relative_np(&cv_entity->cv.cond_handle, &cv_entity->cv.rwlock_mutex_handle, &rel_timespec);
    }
    result = (wait_result != ETIMEDOUT);
  }
  // NOTE(rjf): the internal mutex is dropped *before* re-taking the rw mutex,
  // so that a thread which holds the rw mutex and enters this function cannot
  // deadlock against a woken waiter which is re-taking it.
  pthread_mutex_unlock(&cv_entity->cv.rwlock_mutex_handle);
  if(write_mode)
  {
    pthread_rwlock_wrlock(&rw_mutex_entity->rwmutex_handle);
  }
  else
  {
    pthread_rwlock_rdlock(&rw_mutex_entity->rwmutex_handle);
  }
  return result;
}

internal void
cond_var_signal(CondVar cv)
{
  if(MemoryIsZeroStruct(&cv)) { return; }
  MAC_Entity *cv_entity = (MAC_Entity *)cv.u64[0];
  pthread_cond_signal(&cv_entity->cv.cond_handle);
}

internal void
cond_var_broadcast(CondVar cv)
{
  if(MemoryIsZeroStruct(&cv)) { return; }
  MAC_Entity *cv_entity = (MAC_Entity *)cv.u64[0];
  pthread_cond_broadcast(&cv_entity->cv.cond_handle);
}

//- rjf: cross-process semaphores

internal Semaphore
semaphore_alloc(U32 initial_count, U32 max_count, String8 name)
{
  Temp scratch = scratch_begin(0, 0);
  Semaphore result = {0};
  MAC_Entity *entity = mac_entity_alloc(MAC_EntityKind_Semaphore);
  B32 good = 0;
  if(name.size > 0)
  {
    // rjf: named -> POSIX named semaphore (cross-process)
    String8 ipc_name = mac_ipc_name_from_name(scratch.arena, name);
    sem_t *s = sem_open((char *)ipc_name.str, O_CREAT|O_EXCL, 0600, initial_count);
    if(s == SEM_FAILED && errno == EEXIST)
    {
      // rjf: stale semaphore left behind by a previous run with the same name:
      // unlink it & create a fresh one with the requested count
      sem_unlink((char *)ipc_name.str);
      s = sem_open((char *)ipc_name.str, O_CREAT|O_EXCL, 0600, initial_count);
    }
    if(s == SEM_FAILED)
    {
      s = sem_open((char *)ipc_name.str, 0);
    }
    if(s != SEM_FAILED)
    {
      entity->semaphore.is_named = 1;
      entity->semaphore.is_owner = 1;
      entity->semaphore.posix_handle = s;
      MemoryCopy(entity->semaphore.name, ipc_name.str, Min(ipc_name.size, sizeof(entity->semaphore.name)-1));
      good = 1;
    }
  }
  else
  {
    // rjf: unnamed -> Mach semaphore (task-local)
    semaphore_t s = 0;
    kern_return_t kr = semaphore_create(mach_task_self(), &s, SYNC_POLICY_FIFO, (int)initial_count);
    if(kr == KERN_SUCCESS)
    {
      entity->semaphore.mach_handle = s;
      good = 1;
    }
  }
  if(good)
  {
    result.u64[0] = IntFromPtr(entity);
  }
  else
  {
    mac_entity_release(entity);
  }
  scratch_end(scratch);
  return result;
}

internal void
semaphore_release(Semaphore semaphore)
{
  if(semaphore.u64[0] == 0) { return; }
  MAC_Entity *entity = (MAC_Entity *)PtrFromInt(semaphore.u64[0]);
  if(entity->semaphore.is_named)
  {
    sem_close(entity->semaphore.posix_handle);
    if(entity->semaphore.is_owner)
    {
      sem_unlink(entity->semaphore.name);
    }
  }
  else
  {
    semaphore_destroy(mach_task_self(), entity->semaphore.mach_handle);
  }
  mac_entity_release(entity);
}

internal Semaphore
semaphore_open(String8 name)
{
  Semaphore result = {0};
  if(name.size > 0)
  {
    Temp scratch = scratch_begin(0, 0);
    String8 ipc_name = mac_ipc_name_from_name(scratch.arena, name);
    sem_t *s = sem_open((char *)ipc_name.str, 0);
    if(s != SEM_FAILED)
    {
      MAC_Entity *entity = mac_entity_alloc(MAC_EntityKind_Semaphore);
      entity->semaphore.is_named = 1;
      entity->semaphore.is_owner = 0;
      entity->semaphore.posix_handle = s;
      MemoryCopy(entity->semaphore.name, ipc_name.str, Min(ipc_name.size, sizeof(entity->semaphore.name)-1));
      result.u64[0] = IntFromPtr(entity);
    }
    scratch_end(scratch);
  }
  return result;
}

internal void
semaphore_close(Semaphore semaphore)
{
  if(semaphore.u64[0] == 0) { return; }
  MAC_Entity *entity = (MAC_Entity *)PtrFromInt(semaphore.u64[0]);
  if(entity->semaphore.is_named)
  {
    sem_close(entity->semaphore.posix_handle);
  }
  else
  {
    semaphore_destroy(mach_task_self(), entity->semaphore.mach_handle);
  }
  mac_entity_release(entity);
}

internal B32
semaphore_take(Semaphore semaphore, U64 endt_us)
{
  B32 result = 0;
  if(semaphore.u64[0] != 0)
  {
    MAC_Entity *entity = (MAC_Entity *)PtrFromInt(semaphore.u64[0]);
    if(entity->semaphore.is_named)
    {
      // rjf: POSIX named semaphore; macOS has no sem_timedwait, so timed waits
      // poll sem_trywait until the deadline.
      sem_t *s = entity->semaphore.posix_handle;
      if(endt_us == max_U64)
      {
        int err = MAC_RETRY_ON_EINTR(sem_wait(s));
        result = (err == 0);
      }
      else
      {
        for(;;)
        {
          int err = sem_trywait(s);
          if(err == 0)
          {
            result = 1;
            break;
          }
          if(errno != EAGAIN && errno != EINTR)
          {
            break;
          }
          if(now_time_us() >= endt_us)
          {
            break;
          }
          usleep(500);
        }
      }
    }
    else
    {
      // rjf: Mach semaphore
      semaphore_t s = entity->semaphore.mach_handle;
      for(;;)
      {
        kern_return_t kr = KERN_SUCCESS;
        if(endt_us == max_U64)
        {
          kr = semaphore_wait(s);
        }
        else
        {
          timespec rel_timespec = mac_relative_timespec_from_endt_us(endt_us);
          mach_timespec_t wait_time;
          wait_time.tv_sec  = (unsigned int)rel_timespec.tv_sec;
          wait_time.tv_nsec = (clock_res_t)rel_timespec.tv_nsec;
          kr = semaphore_timedwait(s, wait_time);
        }
        if(kr == KERN_SUCCESS)
        {
          result = 1;
          break;
        }
        if(kr == KERN_ABORTED)
        {
          // rjf: interrupted (e.g. by a signal) -> retry with the remaining time
          continue;
        }
        break;
      }
    }
  }
  return result;
}

internal void
semaphore_drop_count(Semaphore semaphore, U64 count)
{
  if(semaphore.u64[0] == 0) { return; }
  MAC_Entity *entity = (MAC_Entity *)PtrFromInt(semaphore.u64[0]);
  for EachIndex(i, count)
  {
    if(entity->semaphore.is_named)
    {
      int err = MAC_RETRY_ON_EINTR(sem_post(entity->semaphore.posix_handle));
      Assert(err == 0);
    }
    else
    {
      kern_return_t kr = semaphore_signal(entity->semaphore.mach_handle);
      Assert(kr == KERN_SUCCESS);
    }
  }
}

//- rjf: barriers

internal Barrier
barrier_alloc(U64 count)
{
  MAC_Entity *entity = mac_entity_alloc(MAC_EntityKind_Barrier);
  if(entity != 0)
  {
    pthread_mutex_init(&entity->barrier.mutex, 0);
    pthread_cond_init(&entity->barrier.cond, 0);
    entity->barrier.count = Max(count, 1);
    entity->barrier.waiting = 0;
    entity->barrier.generation = 0;
  }
  Barrier result = {IntFromPtr(entity)};
  return result;
}

internal void
barrier_release(Barrier barrier)
{
  MAC_Entity *entity = (MAC_Entity*)PtrFromInt(barrier.u64[0]);
  if(entity != 0)
  {
    pthread_cond_destroy(&entity->barrier.cond);
    pthread_mutex_destroy(&entity->barrier.mutex);
    mac_entity_release(entity);
  }
}

internal void
barrier_wait(Barrier barrier)
{
  MAC_Entity *entity = (MAC_Entity*)PtrFromInt(barrier.u64[0]);
  if(entity != 0)
  {
    pthread_mutex_lock(&entity->barrier.mutex);
    U64 generation = entity->barrier.generation;
    entity->barrier.waiting += 1;
    if(entity->barrier.waiting >= entity->barrier.count)
    {
      // rjf: last thread to arrive -> open the barrier for this generation
      entity->barrier.waiting = 0;
      entity->barrier.generation += 1;
      pthread_cond_broadcast(&entity->barrier.cond);
    }
    else
    {
      while(generation == entity->barrier.generation)
      {
        pthread_cond_wait(&entity->barrier.cond, &entity->barrier.mutex);
      }
    }
    pthread_mutex_unlock(&entity->barrier.mutex);
  }
}

////////////////////////////////
//~ rjf: @per_os_impl Safe Calls

internal void
safe_call(ThreadEntryPointFunctionType *func, ThreadEntryPointFunctionType *fail_handler, void *ptr)
{
  // rjf: push handler to chain
  MAC_SafeCallChain chain = {0};
  SLLStackPush(mac_safe_call_chain, &chain);
  chain.fail_handler = fail_handler;
  chain.ptr = ptr;

  // rjf: set up sig handler info
  struct sigaction new_act = {0};
  new_act.sa_sigaction = mac_safe_call_sig_handler;
  new_act.sa_flags = SA_SIGINFO;
  int signals_to_handle[] =
  {
    SIGILL, SIGFPE, SIGSEGV, SIGBUS, SIGTRAP,
  };
  struct sigaction og_act[ArrayCount(signals_to_handle)] = {0};

  // rjf: attach handler info for all signals
  for(U32 i = 0; i < ArrayCount(signals_to_handle); i += 1)
  {
    sigaction(signals_to_handle[i], &new_act, &og_act[i]);
  }

  // rjf: call function
  func(ptr);

  // rjf: reset handler info for all signals
  for(U32 i = 0; i < ArrayCount(signals_to_handle); i += 1)
  {
    sigaction(signals_to_handle[i], &og_act[i], 0);
  }
  mac_safe_call_chain = chain.next;
}

////////////////////////////////
//~ rjf: @per_os_impl File System (Implemented Per-OS)

//- rjf: files

internal File
file_open(AccessFlags flags, String8 path)
{
  Temp scratch = scratch_begin(0, 0);

  int mac_flags = 0;

  if(flags & AccessFlag_Read && flags & AccessFlag_Write)
  {
    mac_flags = O_RDWR;
  }
  else if(flags & AccessFlag_Write)
  {
    mac_flags = O_WRONLY;
  }
  else if(flags & AccessFlag_Read)
  {
    mac_flags = O_RDONLY;
  }

  if(flags & AccessFlag_Append)
  {
    mac_flags |= O_APPEND;
  }
  else if(flags & AccessFlag_Write)
  {
    mac_flags |= O_TRUNC;
  }

  if(flags & (AccessFlag_Write|AccessFlag_Append))
  {
    mac_flags |= O_CREAT;
  }

  if(flags & AccessFlag_CreateNew) { mac_flags |= O_CREAT|O_EXCL; }

  if(!(flags & AccessFlag_Inherited)) { mac_flags |= O_CLOEXEC; }

  String8 path_copy = push_str8_copy(scratch.arena, path);
  int fd = open((char *)path_copy.str, mac_flags, 0755);
  File handle = {0};
  if(fd != -1)
  {
    handle.u64[0] = fd;
  }

  scratch_end(scratch);
  return handle;
}

internal void
file_close(File file)
{
  if(file_match(file, file_zero())) { return; }
  int fd = (int)file.u64[0];
  close(fd);
}

internal FilePair
file_pipe_make(B32 read_inherited, B32 write_inherited)
{
  FilePair result = {0};
  int fds[2] = {0};
  if(pipe(fds) == 0)
  {
    if(!read_inherited)  { fcntl(fds[0], F_SETFD, FD_CLOEXEC); }
    if(!write_inherited) { fcntl(fds[1], F_SETFD, FD_CLOEXEC); }
    result.read.u64[0] = fds[0];
    result.write.u64[0] = fds[1];
  }
  return result;
}

internal U64
file_pipe_read(File file, void *out_data, U64 size)
{
  ssize_t read_size = MAC_RETRY_ON_EINTR(read((int)file.u64[0], out_data, size));
  return read_size > 0 ? (U64)read_size : 0;
}

internal U64
file_pipe_write(File file, void *data, U64 size)
{
  ssize_t write_size = MAC_RETRY_ON_EINTR(write((int)file.u64[0], data, size));
  return write_size > 0 ? (U64)write_size : 0;
}

internal U64
file_pipe_bytes_available(File file)
{
  int size = 0;
  B32 is_ok = !file_match(file, file_zero()) && ioctl((int)file.u64[0], FIONREAD, &size) == 0;
  return is_ok && size > 0 ? (U64)size : 0;
}

internal B32
file_pipe_is_end(File file)
{
  struct pollfd poll_fd = { .fd = (int)file.u64[0], .events = POLLIN|POLLHUP };
  poll(&poll_fd, 1, 0);
  return !!(poll_fd.revents & (POLLHUP|POLLERR|POLLNVAL));
}

internal U64
file_read(File file, Rng1U64 rng, void *out_data)
{
  if(file_match(file, file_zero())) { return 0; }
  int fd = (int)file.u64[0];
  U64 total_num_bytes_to_read = dim_1u64(rng);
  U64 total_num_bytes_read = 0;
  U64 total_num_bytes_left_to_read = total_num_bytes_to_read;
  for(;total_num_bytes_left_to_read > 0;)
  {
    ssize_t read_result = pread(fd, (U8 *)out_data + total_num_bytes_read, total_num_bytes_left_to_read, rng.min + total_num_bytes_read);
    if(read_result > 0)
    {
      total_num_bytes_read += read_result;
      total_num_bytes_left_to_read -= read_result;
    }
    else if(read_result == 0)
    {
      // rjf: end of file
      break;
    }
    else if(errno != EINTR)
    {
      break;
    }
  }
  return total_num_bytes_read;
}

internal U64
file_write(File file, Rng1U64 rng, void *data)
{
  if(file_match(file, file_zero())) { return 0; }
  int fd = (int)file.u64[0];
  U64 total_num_bytes_to_write = dim_1u64(rng);
  U64 total_num_bytes_written = 0;
  U64 total_num_bytes_left_to_write = total_num_bytes_to_write;
  for(;total_num_bytes_left_to_write > 0;)
  {
    ssize_t write_result = pwrite(fd, (U8 *)data + total_num_bytes_written, total_num_bytes_left_to_write, rng.min + total_num_bytes_written);
    if(write_result >= 0)
    {
      total_num_bytes_written += write_result;
      total_num_bytes_left_to_write -= write_result;
    }
    else if(errno != EINTR)
    {
      break;
    }
  }
  return total_num_bytes_written;
}

internal B32
file_set_times(File file, DateTime date_time)
{
  if(file_match(file, file_zero())) { return 0; }
  int fd = (int)file.u64[0];
  timespec time = mac_timespec_from_date_time(date_time);
  timespec times[2] = {time, time};
  int futimens_result = futimens(fd, times);
  B32 good = (futimens_result != -1);
  return good;
}

internal FileProperties
properties_from_file(File file)
{
  if(file_match(file, file_zero())) { return (FileProperties){0}; }
  int fd = (int)file.u64[0];
  struct stat fd_stat = {0};
  int fstat_result = fstat(fd, &fd_stat);
  FileProperties props = {0};
  if(fstat_result != -1)
  {
    props = mac_file_properties_from_stat(&fd_stat);
  }
  return props;
}

internal FileID
id_from_file(File file)
{
  if(file_match(file, file_zero())) { return (FileID){0}; }
  int fd = (int)file.u64[0];
  struct stat fd_stat = {0};
  int fstat_result = fstat(fd, &fd_stat);
  FileID id = {0};
  if(fstat_result != -1)
  {
    id.v[0] = fd_stat.st_dev;
    id.v[1] = fd_stat.st_ino;
  }
  return id;
}

internal B32
file_set_size(File file, U64 size)
{
  return size <= max_S64 && ftruncate((int)file.u64[0], (off_t)size) == 0;
}

internal B32
file_flush(File file)
{
  return fsync((int)file.u64[0]) == 0;
}

internal B32
replace_file_path(String8 dst, String8 src)
{
  Temp scratch = scratch_begin(0,0);
  char *dst_cstr = (char *)push_str8_copy(scratch.arena, dst).str;
  char *src_cstr = (char *)push_str8_copy(scratch.arena, src).str;
  B32   result   = rename(src_cstr, dst_cstr) == 0;
  scratch_end(scratch);
  return result;
}

internal B32
memory_placeholders_supported(void)
{
  return 0;
}

internal void *
reserve_memory_placeholders(U64 size, U64 block_size)
{
  Assert(size > 0);
  Assert(size == (size_t)size);
  Assert(block_size > 0);
  Assert((size % block_size) == 0);
  Assert((block_size % get_system_info()->page_size) == 0);
  return reserve_memory(size);
}

internal B32
release_memory_placeholders(void *ptr, U64 size, U64 block_size)
{
  return size && block_size && munmap(ptr, size) == 0;
}

internal B32
unmap_memory_preserve_placeholder(void *ptr, U64 size)
{
  Assert(ptr != 0);
  Assert(size > 0);
  Assert(size == (size_t)size);
  Assert((IntFromPtr(ptr) % get_system_info()->page_size) == 0);
  Assert((size % get_system_info()->page_size) == 0);
  return mmap(ptr, size, PROT_NONE, MAP_FIXED|MAP_PRIVATE|MAP_ANONYMOUS, -1, 0) == ptr;
}

internal void *
shared_memory_view_replace_placeholder(SharedMemory handle, void *ptr, Rng1U64 range, AccessFlags flags)
{
  Assert(range.max > range.min);
  Assert(range.max < max_S64);
  Assert(dim_1u64(range) == (size_t)dim_1u64(range));
  Assert((range.min % get_system_info()->page_size) == 0);
  Assert((range.max % get_system_info()->page_size) == 0);
  Assert((IntFromPtr(ptr) % get_system_info()->page_size) == 0);
  Assert((flags == AccessFlag_Read) || (flags == (AccessFlag_Read|AccessFlag_Write)));
  int   protection = flags == AccessFlag_Read ? PROT_READ : PROT_READ|PROT_WRITE;
  void *result     = mmap(ptr, dim_1u64(range), protection, MAP_FIXED|MAP_SHARED, (int)handle.u64[0], range.min);
  return result == MAP_FAILED ? 0 : result;
}

internal U64
get_available_commit_memory(void)
{
  U64 result = 0;
  {
    mach_port_t host = mach_host_self();
    vm_statistics64_data_t vm_stats = {0};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    kern_return_t kr = host_statistics64(host, HOST_VM_INFO64, (host_info64_t)&vm_stats, &count);
    mach_port_deallocate(mach_task_self(), host);
    if(kr == KERN_SUCCESS)
    {
      result = ((U64)vm_stats.free_count + (U64)vm_stats.inactive_count) * get_system_info()->page_size;
    }
  }
  if(result == 0)
  {
    result = get_system_info()->physical_memory_size;
  }
  return result;
}

internal B32
split_memory_placeholder(void *ptr, U64 size, U64 first_size)
{
  return first_size > 0 && first_size <= size;
}

internal B32
coalesce_memory_placeholders(void *ptr, U64 size)
{
  return 1;
}

internal void *
file_map_view_replace_placeholder(FileMap map, void *ptr, Rng1U64 range)
{
  void *result = mmap(ptr, dim_1u64(range), PROT_READ, MAP_FIXED|MAP_PRIVATE, (int)map.u64[0], range.min);
  return result == MAP_FAILED ? 0 : result;
}

internal void
prefetch_memory_ranges(U64 count, Rng1U64 *ranges)
{
  U64 page = get_system_info()->page_size;
  for EachIndex(i, count) {
    U64 min = AlignDownPow2(ranges[i].min, page), max = AlignPow2(ranges[i].max, page);
    if (max > min) { madvise(PtrFromInt(min), max-min, MADV_WILLNEED); }
  }
}

internal B32
memory_read_fault_handler_set(MemoryReadFaultFunction *func, void *user_data)
{
  // NOTE(rjf): on-demand memory (user-space page fault handling) is not
  // supported on macOS; installing a handler fails, clearing one succeeds.
  B32 result = (func == 0);
  return result;
}

internal B32
file_reserve_size(File file, U64 size)
{
  if(file_match(file, file_zero())) { return 0; }
  if(size > max_S64) { return 0; }
  int fd = (int)file.u64[0];
  B32 result = 0;
  struct stat st = {0};
  if(fstat(fd, &st) == 0)
  {
    if(st.st_size >= (off_t)size)
    {
      result = 1;
    }
    else
    {
      // NOTE(rjf): F_PEOFPOSMODE allocates `fst_length` bytes past the current
      // end of the file, without changing the file's size.
      fstore_t store = {0};
      store.fst_flags   = F_ALLOCATEALL;
      store.fst_posmode = F_PEOFPOSMODE;
      store.fst_offset  = 0;
      store.fst_length  = (off_t)size - st.st_size;
      result = (fcntl(fd, F_PREALLOCATE, &store) != -1);
    }
  }
  return result;
}

internal B32
delete_file_at_path(String8 path)
{
  Temp scratch = scratch_begin(0, 0);
  B32 result = 0;
  String8 path_copy = push_str8_copy(scratch.arena, path);
  if(remove((char *)path_copy.str) != -1)
  {
    result = 1;
  }
  scratch_end(scratch);
  return result;
}

internal B32
copy_file_path(String8 dst, String8 src)
{
  Temp scratch = scratch_begin(0, 0);
  char *dst_cstr = (char *)push_str8_copy(scratch.arena, dst).str;
  char *src_cstr = (char *)push_str8_copy(scratch.arena, src).str;
  // NOTE(rjf): COPYFILE_UNLINK replaces an existing destination (the Linux
  // implementation truncates it), so the result is always an exact copy.
  B32 result = (copyfile(src_cstr, dst_cstr, 0, COPYFILE_DATA|COPYFILE_UNLINK) == 0);
  scratch_end(scratch);
  return result;
}

internal B32
move_file_path(String8 dst, String8 src)
{
  B32 good = 0;
  Temp scratch = scratch_begin(0, 0);
  {
    char *src_cstr = (char *)str8_copy(scratch.arena, src).str;
    char *dst_cstr = (char *)str8_copy(scratch.arena, dst).str;
    int rename_result = rename(src_cstr, dst_cstr);
    good = (rename_result != -1);
  }
  scratch_end(scratch);
  return good;
}

internal String8
full_path_from_path(Arena *arena, String8 path)
{
  Temp scratch = scratch_begin(&arena, 1);
  String8 path_copy = str8_copy(scratch.arena, path);
  char buffer[PATH_MAX] = {0};
  String8 result = {0};
  if(realpath((char *)path_copy.str, buffer) != 0)
  {
    result = str8_copy(arena, str8_cstring(buffer));
  }
  else if(path.size > 0 && path.str[0] == '/')
  {
    // rjf: path does not exist (yet) -> already absolute, keep as-is
    result = str8_copy(arena, path);
  }
  else
  {
    // rjf: path does not exist (yet) -> make absolute relative to the cwd
    String8 cwd = get_current_path(scratch.arena);
    result = str8f(arena, "%S/%S", cwd, path);
  }
  scratch_end(scratch);
  return result;
}

internal B32
file_path_exists(String8 path)
{
  Temp scratch = scratch_begin(0, 0);
  String8 path_copy = push_str8_copy(scratch.arena, path);
  int access_result = access((char *)path_copy.str, F_OK);
  B32 result = 0;
  if(access_result == 0)
  {
    result = 1;
  }
  scratch_end(scratch);
  return result;
}

internal B32
folder_path_exists(String8 path)
{
  Temp scratch = scratch_begin(0, 0);
  B32 exists = 0;
  String8  path_copy = str8_copy(scratch.arena, path);
  DIR *handle = opendir((char *)path_copy.str);
  if(handle)
  {
    closedir(handle);
    exists = 1;
  }
  scratch_end(scratch);
  return exists;
}

internal FileProperties
properties_from_file_path(String8 path)
{
  Temp scratch = scratch_begin(0, 0);
  String8 path_copy = str8_copy(scratch.arena, path);
  struct stat f_stat = {0};
  int stat_result = stat((char *)path_copy.str, &f_stat);
  FileProperties props = {0};
  if(stat_result != -1)
  {
    props = mac_file_properties_from_stat(&f_stat);
  }
  scratch_end(scratch);
  return props;
}

//- rjf: file maps

internal FileMap
file_map_open(AccessFlags flags, File file)
{
  FileMap map = {file.u64[0]};
  return map;
}

internal void
file_map_close(FileMap map)
{
  // NOTE(rjf): nothing to do; `map` handles are the same as `file` handles in
  // the macOS implementation (on Windows they require separate handles)
}

internal void *
file_map_view_open(FileMap map, AccessFlags flags, Rng1U64 range)
{
  if(MemoryIsZeroStruct(&map)) { return 0; }
  int fd = (int)map.u64[0];
  int prot_flags = 0;
  if(flags & AccessFlag_Write) { prot_flags |= PROT_WRITE; }
  if(flags & AccessFlag_Read)  { prot_flags |= PROT_READ; }
  int map_flags = MAP_PRIVATE;
  void *base = mmap(0, dim_1u64(range), prot_flags, map_flags, fd, range.min);
  if(base == MAP_FAILED)
  {
    base = 0;
  }
  return base;
}

internal void
file_map_view_close(FileMap map, void *ptr, Rng1U64 range)
{
  munmap(ptr, dim_1u64(range));
}

//- rjf: directory iteration

internal FileIter *
file_iter_begin(Arena *arena, String8 path, FileIterFlags flags)
{
  FileIter *base_iter = push_array(arena, FileIter, 1);
  base_iter->flags = flags;
  MAC_FileIter *iter = (MAC_FileIter *)base_iter->memory;
  {
    String8 path_copy = push_str8_copy(arena, path);
    iter->dir = opendir((char *)path_copy.str);
    iter->path = path_copy;
  }
  return base_iter;
}

internal B32
file_iter_next(Arena *arena, FileIter *iter, FileInfo *info_out)
{
  B32 good = 0;
  MAC_FileIter *mac_iter = (MAC_FileIter *)iter->memory;
  for(;mac_iter->dir != 0;)
  {
    // rjf: get next entry
    mac_iter->dp = readdir(mac_iter->dir);
    good = (mac_iter->dp != 0);

    // rjf: unpack entry info
    struct stat st = {0};
    int stat_result = 0;
    if(good)
    {
      Temp scratch = scratch_begin(&arena, 1);
      String8 full_path = push_str8f(scratch.arena, "%S/%s", mac_iter->path, mac_iter->dp->d_name);
      stat_result = stat((char *)full_path.str, &st);
      scratch_end(scratch);
    }

    // rjf: determine if filtered
    B32 filtered = 0;
    if(good)
    {
      char *name = mac_iter->dp->d_name;
      B32 is_dot_entry = ((name[0] == '.' && name[1] == 0) ||
                          (name[0] == '.' && name[1] == '.' && name[2] == 0));
      B32 is_hidden = (name[0] == '.');
      B32 is_folder = (stat_result != -1 && S_ISDIR(st.st_mode));
      filtered = (is_dot_entry ||
                  (is_hidden && iter->flags & FileIterFlag_SkipHiddenFiles) ||
                  (is_folder && iter->flags & FileIterFlag_SkipFolders) ||
                  (!is_folder && iter->flags & FileIterFlag_SkipFiles));
    }

    // rjf: output & exit, if good & unfiltered
    if(good && !filtered)
    {
      info_out->name = push_str8_copy(arena, str8_cstring(mac_iter->dp->d_name));
      if(stat_result != -1)
      {
        info_out->props = mac_file_properties_from_stat(&st);
      }
      break;
    }

    // rjf: exit if not good
    if(!good)
    {
      break;
    }
  }
  return good;
}

internal void
file_iter_end(FileIter *iter)
{
  MAC_FileIter *mac_iter = (MAC_FileIter *)iter->memory;
  if(mac_iter->dir != 0)
  {
    closedir(mac_iter->dir);
    mac_iter->dir = 0;
  }
}

//- rjf: directory creation

internal B32
make_directory(String8 path)
{
  Temp scratch = scratch_begin(0, 0);
  B32 result = 0;
  String8 path_copy = push_str8_copy(scratch.arena, path);
  if(mkdir((char *)path_copy.str, 0755) != -1)
  {
    result = 1;
  }
  else
  {
    // match windows behavior
    result = file_path_exists(path);
  }
  scratch_end(scratch);
  return result;
}

////////////////////////////////
//~ rjf: @per_os_impl Aborting

internal void
abort_self(U64 exit_code)
{
  exit((int)exit_code);
}

////////////////////////////////
//~ rjf: @per_os_impl Process Info

internal ProcessInfo *
get_process_info(void)
{
  return &mac_state.process_info;
}

internal String8
get_current_path(Arena *arena)
{
  char *cwdir = getcwd(0, 0);
  String8 string = str8_copy(arena, str8_cstring(cwdir));
  free(cwdir);
  return string;
}

internal U32
get_process_start_time_unix(void)
{
  U64 start_time = 0;
  struct kinfo_proc info = {0};
  if(mac_kinfo_proc_from_pid(getpid(), &info))
  {
    start_time = (U64)info.kp_proc.p_starttime.tv_sec;
  }
  return (U32)start_time;
}

////////////////////////////////
//~ rjf: @per_os_impl Child Processes

internal Process
process_launch(ProcessLaunchParams *params)
{
  Process handle = {0};
  posix_spawn_file_actions_t file_actions = {0};
  int file_actions_init_code = posix_spawn_file_actions_init(&file_actions);
  if(file_actions_init_code == 0)
  {
    Temp scratch = scratch_begin(0, 0);
    if(params->path.size != 0)
    {
      int chdir_code = posix_spawn_file_actions_addchdir_np(&file_actions, (char *)push_cstr(scratch.arena, params->path).str);
      Assert(chdir_code == 0);
    }
    if(!file_match(params->stdout_file, file_zero()))
    {
      int stdout_code = posix_spawn_file_actions_adddup2(&file_actions, (int)params->stdout_file.u64[0], STDOUT_FILENO);
      Assert(stdout_code == 0);
    }
    if(!file_match(params->stderr_file, file_zero()))
    {
      int stderr_code = posix_spawn_file_actions_adddup2(&file_actions, (int)params->stderr_file.u64[0], STDERR_FILENO);
      Assert(stderr_code == 0);
    }
    if(!file_match(params->stdin_file, file_zero()))
    {
      int stdin_code = posix_spawn_file_actions_adddup2(&file_actions, (int)params->stdin_file.u64[0], STDIN_FILENO);
      Assert(stdin_code == 0);
    }
    File std_files[] = { params->stdout_file, params->stderr_file, params->stdin_file };
    for EachIndex(i, ArrayCount(std_files))
    {
      int fd = (int)std_files[i].u64[0];
      B32 is_unique = !file_match(std_files[i], file_zero());
      for EachIndex(j, i) { is_unique &= !file_match(std_files[i], std_files[j]); }
      if(is_unique && fd > STDERR_FILENO)
      {
        int close_code = posix_spawn_file_actions_addclose(&file_actions, fd);
        Assert(close_code == 0);
      }
    }
    posix_spawnattr_t attr = {0};
    int attr_init_code = posix_spawnattr_init(&attr);
    if(attr_init_code == 0)
    {
      if(params->new_console || params->process_group.u64[0] != 0)
      {
        posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
        posix_spawnattr_setpgroup(&attr, 0);
      }

      // package argv
      char **argv = push_array(scratch.arena, char *, params->cmd_line.node_count + 1);
      {
        argv[0] = (char *)push_cstr(scratch.arena, params->cmd_line.first->string).str;
        U64 arg_idx = 1;
        for EachNode(n, String8Node, params->cmd_line.first->next)
        {
          argv[arg_idx] = (char *)push_cstr(scratch.arena, n->string).str;
          arg_idx += 1;
        }
      }

      // package envp
      char **envp = 0;
      if(params->inherit_env)
      {
        envp = mac_state.default_env;
      }
      else
      {
        envp = push_array(scratch.arena, char *, params->env.node_count + 1);
        U64 env_idx = 0;
        for EachNode(n, String8Node, params->env.first)
        {
          envp[env_idx] = (char *)push_cstr(scratch.arena, n->string).str;
          env_idx += 1;
        }
      }

      // spawn process
      pid_t pid = 0;
      int spawn_code = posix_spawnp(&pid, argv[0], &file_actions, &attr, argv, envp);
      if(spawn_code == 0)
      {
        handle.u64[0] = (U64)pid;
        if(params->process_group.u64[0] != 0 && !process_group_add(params->process_group, handle))
        {
          kill(pid, SIGKILL);
          waitpid(pid, 0, 0);
          MemoryZeroStruct(&handle);
        }
      }

      // clean up attributes
      int attr_destroy_code = posix_spawnattr_destroy(&attr);
    }
    scratch_end(scratch);

    // clean up file actions
    int file_actions_destroy_code = posix_spawn_file_actions_destroy(&file_actions);
  }
  return handle;
}

internal U64
pid_from_process(Process process)
{
  U64 result = process.u64[0];
  return result;
}

internal B32
process_poll(Process process, U64 *exit_code_out)
{
  if(process_match(process, process_zero())) { return 0; }
  siginfo_t info = {0};
  B32 result = waitid(P_PID, (id_t)process.u64[0], &info, WEXITED|WNOHANG|WNOWAIT) == 0 && info.si_pid != 0;
  if(result && exit_code_out != 0)
  {
    *exit_code_out = info.si_code == CLD_EXITED ? (U64)info.si_status : (U64)info.si_status + 128;
  }
  return result;
}

internal B32
process_is_active(Process process)
{
  B32 result = !process_match(process, process_zero()) &&
    !process_poll(process, 0) && kill((pid_t)process.u64[0], 0) == 0;
  return result;
}

internal B32
process_join(Process process, U64 endt_us, U64 *exit_code_out)
{
  B32 result = 0;

  pid_t pid = (pid_t)process.u64[0];
  for(;;)
  {
    int status = 0;
    pid_t wait_result = MAC_RETRY_ON_EINTR(waitpid(pid, &status, (endt_us == max_U64) ? 0 : WNOHANG));

    if((wait_result == pid) && (WIFEXITED(status) || WIFSIGNALED(status)))
    {
      result = 1;
      if(exit_code_out != 0)
      {
        if     (WIFEXITED(status))   { *exit_code_out = WEXITSTATUS(status); }
        else if(WIFSIGNALED(status)) { *exit_code_out = WTERMSIG(status) + 128; }
      }
      break;
    }

    if(wait_result == -1) { break; }
    if(endt_us == 0)      { break; }

    U64 now_us = now_time_us();
    if(now_us >= endt_us) { break; }

    U64 left_us  = endt_us - now_us;
    U64 sleep_us = Min(left_us, Thousand(1));
    usleep((useconds_t)sleep_us);
  }
  return result;
}

internal void
process_detach(Process process)
{
  // no need to close pid
}

internal B32
process_kill(Process process)
{
  int error_code = kill((pid_t)process.u64[0], SIGKILL);
  B32 is_killed = error_code == 0;
  return is_killed;
}

internal B32
process_send_ctrl_c(Process process)
{
  B32 result = !process_match(process, process_zero()) && kill((pid_t)process.u64[0], SIGINT) == 0;
  return result;
}

internal ProcessGroup
process_group_make(B32 kill_on_close)
{
  MAC_ProcessGroup *group = malloc(sizeof(*group));
  if(group != 0) { *group = (MAC_ProcessGroup){ .kill_on_close = kill_on_close }; }
  return (ProcessGroup){ .u64[0] = (U64)group };
}

internal B32
process_group_add(ProcessGroup group, Process process)
{
  MAC_ProcessGroup *mac_group = (MAC_ProcessGroup *)group.u64[0];
  if(mac_group == 0 || mac_group->pid != 0 || process_match(process, process_zero())) { return 0; }
  mac_group->pid = (pid_t)process.u64[0];
  return 1;
}

internal void
process_group_close(ProcessGroup group)
{
  MAC_ProcessGroup *mac_group = (MAC_ProcessGroup *)group.u64[0];
  if(mac_group != 0)
  {
    if(mac_group->kill_on_close && mac_group->pid != 0)
    {
      kill(-mac_group->pid, SIGKILL);
    }
    free(mac_group);
  }
}

////////////////////////////////
//~ rjf: @per_os_impl Dynamically-Loaded Libraries

internal Library
library_open(String8 path)
{
  Temp scratch = scratch_begin(0, 0);
  char *path_cstr = (char *)str8_copy(scratch.arena, path).str;
  void *so = dlopen(path_cstr, RTLD_LAZY|RTLD_LOCAL);
  Library lib = { (U64)so };
  scratch_end(scratch);
  return lib;
}

internal void
library_close(Library lib)
{
  void *so = (void *)lib.u64[0];
  dlclose(so);
}

internal VoidProc *
library_load_proc(Library lib, String8 name)
{
  Temp scratch = scratch_begin(0, 0);
  void *so = (void *)lib.u64[0];
  char *name_cstr = (char *)str8_copy(scratch.arena, name).str;
  VoidProc *proc = (VoidProc *)dlsym(so, name_cstr);
  scratch_end(scratch);
  return proc;
}

////////////////////////////////
//~ rjf: Entry Point

internal void
mac_signal_handler(int sig, siginfo_t *info, void *arg)
{
  // crash handler
  {
    local_persist volatile U32 first = 0;
    if (ins_atomic_u32_eval_cond_assign(&first, 1, 0) != 0)
    {
      for(;;)
      {
        sleep(UINT32_MAX);
      }
    }

    local_persist void *ips[4096];
    int ips_count = backtrace(ips, ArrayCount(ips));

    fprintf(stderr, "A fatal signal was received: %s (%d). The process is terminating.\n", strsignal(sig), sig);
    fprintf(stderr, "Create a new issue with this report at %s.\n\n", BUILD_ISSUES_LINK_STRING_LITERAL);
    fprintf(stderr, "Callstack:\n");
    for EachIndex(i, ips_count)
    {
      Dl_info dl = {0};
      int dladdr_ok = dladdr(ips[i], &dl);
      char *image_path = (dladdr_ok && dl.dli_fname != 0) ? (char *)dl.dli_fname : "";
      String8 module = str8_skip_last_slash(str8_cstring(image_path));

      // rjf: try to symbolize via `atos` (part of the Xcode command line tools)
      B32 symbolized = 0;
      if(dladdr_ok && dl.dli_fname != 0 && dl.dli_fbase != 0)
      {
        char cmd[2048];
        snprintf(cmd, sizeof(cmd), "atos -o '%s' -l 0x%llx 0x%llx 2>/dev/null",
                 dl.dli_fname, (unsigned long long)(uintptr_t)dl.dli_fbase, (unsigned long long)(uintptr_t)ips[i]);
        FILE *f = popen(cmd, "r");
        if(f)
        {
          char line[1024] = {0};
          if(fgets(line, sizeof(line), f))
          {
            String8 text = str8_cstring_capped(line, line + sizeof(line));
            for(;text.size > 0 && (text.str[text.size-1] == '\n' || text.str[text.size-1] == '\r');) { text.size -= 1; }
            if(text.size > 0)
            {
              fprintf(stderr, "%d. [0x%016llx] %.*s, %.*s\n", (int)(i+1), (unsigned long long)(uintptr_t)ips[i], (int)module.size, module.str, (int)text.size, text.str);
              symbolized = 1;
            }
          }
          pclose(f);
        }
      }

      // rjf: `atos` unavailable or unhelpful -> fall back to dladdr's nearest symbol
      if(!symbolized)
      {
        if(dladdr_ok && dl.dli_sname != 0)
        {
          fprintf(stderr, "%d. [0x%016llx] %.*s, %s + 0x%llx\n", (int)(i+1), (unsigned long long)(uintptr_t)ips[i], (int)module.size, module.str, dl.dli_sname, (unsigned long long)((uintptr_t)ips[i] - (uintptr_t)dl.dli_saddr));
        }
        else
        {
          fprintf(stderr, "%d. [0x%016llx] %.*s\n", (int)(i+1), (unsigned long long)(uintptr_t)ips[i], (int)module.size, module.str);
        }
      }
    }
    fprintf(stderr, "\nVersion: %s%s\n\n", BUILD_VERSION_STRING_LITERAL, BUILD_GIT_HASH_STRING_LITERAL_APPEND);

    _exit(1);
  }
}

int
main(int argc, char **argv)
{
  // install signal handler for the crash call stacks
  {
    struct sigaction handler = { .sa_sigaction = mac_signal_handler, .sa_flags = SA_SIGINFO, };
    sigfillset(&handler.sa_mask);
    sigaction(SIGILL, &handler, NULL);
    sigaction(SIGTRAP, &handler, NULL);
    sigaction(SIGABRT, &handler, NULL);
    sigaction(SIGFPE, &handler, NULL);
    sigaction(SIGBUS, &handler, NULL);
    sigaction(SIGSEGV, &handler, NULL);
    sigaction(SIGQUIT, &handler, NULL);
  }

  //- rjf: set up OS layer
  {
    //- rjf: get statically-allocated system/process info
    {
      SystemInfo *info = &mac_state.system_info;
      long processor_count = sysconf(_SC_NPROCESSORS_ONLN);
      info->logical_processor_count = (processor_count > 0) ? (U32)processor_count : 1;
      info->page_size               = (U64)getpagesize();
      info->large_page_size         = MB(2);
      info->allocation_granularity  = info->page_size;
      {
        U64 memsize = 0;
        size_t memsize_size = sizeof(memsize);
        if(sysctlbyname("hw.memsize", &memsize, &memsize_size, 0, 0) != 0 || memsize == 0)
        {
          long pages = sysconf(_SC_PHYS_PAGES);
          memsize = (pages > 0) ? (U64)pages * info->page_size : 0;
        }
        info->physical_memory_size = memsize;
      }
    }
    {
      ProcessInfo *info = &mac_state.process_info;
      info->pid = (U32)getpid();
    }

    //- rjf: set up thread context
    TCTX *tctx = tctx_alloc();
    tctx_select(tctx);

    //- rjf: set up dynamically allocated state
    mac_state.arena = arena_alloc();
    mac_state.entity_arena = arena_alloc();
    pthread_mutex_init(&mac_state.entity_mutex, 0);

    // cache default environment
    {
      char **environ_ptr = *_NSGetEnviron();
      U64 env_count = 0;
      if(environ_ptr != 0)
      {
        for(; environ_ptr[env_count] != 0; env_count += 1) {}
      }

      char **default_env = push_array(mac_state.arena, char *, env_count+1);
      for EachIndex(idx, env_count)
      {
        String8 entry = str8_copy(mac_state.arena, str8_cstring(environ_ptr[idx]));
        default_env[idx] = (char *)entry.str;
        str8_list_push(mac_state.arena, &mac_state.process_info.environment, entry);
      }
      default_env[env_count] = 0;

      mac_state.default_env_count = env_count;
      mac_state.default_env       = default_env;
    }

    //- rjf: grab dynamically allocated system info
    {
      Temp scratch = scratch_begin(0, 0);
      SystemInfo *info = &mac_state.system_info;

      // rjf: get machine name
      B32 got_final_result = 0;
      U8 *buffer = 0;
      int size = 0;
      for(S64 cap = 4096, r = 0; r < 4; cap *= 2, r += 1)
      {
        scratch_end(scratch);
        buffer = push_array(scratch.arena, U8, cap);
        int gethostname_result = gethostname((char*)buffer, cap);
        size = cstring8_length(buffer);
        if(gethostname_result == 0 && size < cap)
        {
          got_final_result = 1;
          break;
        }
      }

      // rjf: save name to info
      if(got_final_result && size > 0)
      {
        info->machine_name.size = size;
        info->machine_name.str = push_array_no_zero(mac_state.arena, U8, info->machine_name.size + 1);
        MemoryCopy(info->machine_name.str, buffer, info->machine_name.size);
        info->machine_name.str[info->machine_name.size] = 0;
      }

      scratch_end(scratch);
    }

    //- rjf: grab dynamically allocated process info
    {
      Temp scratch = scratch_begin(0, 0);
      ProcessInfo *info = &mac_state.process_info;

      // rjf: grab binary path
      {
        // rjf: get self string
        B32 got_final_result = 0;
        U8 *buffer = 0;
        for(uint32_t cap = PATH_MAX, r = 0; r < 4; r += 1)
        {
          scratch_end(scratch);
          buffer = push_array(scratch.arena, U8, cap);
          uint32_t size = cap;
          if(_NSGetExecutablePath((char *)buffer, &size) == 0)
          {
            got_final_result = 1;
            break;
          }
          cap = size+1;
        }

        // rjf: resolve to a real path
        String8 full_name = {0};
        if(got_final_result)
        {
          char resolved[PATH_MAX] = {0};
          if(realpath((char *)buffer, resolved) != 0)
          {
            full_name = str8_cstring(resolved);
          }
          else
          {
            full_name = str8_cstring((char *)buffer);
          }
        }

        // rjf: save
        if(full_name.size > 0)
        {
          info->binary_file_path = push_str8_copy(mac_state.arena, full_name);
          info->binary_path = str8_chop_last_slash(info->binary_file_path);
        }
      }

      // rjf: grab initial directory
      {
        info->initial_path = get_current_path(mac_state.arena);
      }

      // rjf: grab program/user data paths
      {
        char *home = getenv("HOME");
        if(home == 0 || home[0] == 0)
        {
          home = "/tmp";
        }
        info->user_program_config_data_path = str8f(mac_state.arena, "%s/Library/Application Support", home);
        info->user_program_cache_data_path  = str8f(mac_state.arena, "%s/Library/Caches", home);
        info->user_program_logs_data_path   = str8f(mac_state.arena, "%s/Library/Logs", home);
      }

      scratch_end(scratch);
    }
  }

  //- rjf: call into "real" entry point
  main_thread_base_entry_point(argc, argv);
  return 0;
}
