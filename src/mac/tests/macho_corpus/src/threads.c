// Threads and thread-local storage: several pthreads each touch initialized
// and zero-initialized _Thread_local variables; the main thread joins them.
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <stdatomic.h>

_Thread_local int32_t t_id = -1;
_Thread_local int64_t t_sum;
_Thread_local char t_name[32] = "worker";
static _Thread_local int32_t t_static_counter = 100;

static atomic_int g_done = 0;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int64_t g_total = 0;

typedef struct Job Job;
struct Job
{
  int32_t id;
  int32_t iterations;
  int64_t result;
};

static int64_t
work_step(int32_t i)
{
  t_sum += i * (t_id + 1);
  t_static_counter += 1;
  return t_sum;
}

static void *
worker(void *arg)
{
  Job *job = (Job *)arg;
  t_id = job->id;
  snprintf(t_name, sizeof(t_name), "worker-%d", t_id);
  for(int32_t i = 0; i < job->iterations; i += 1)
  {
    job->result = work_step(i);
  }
  pthread_mutex_lock(&g_lock);
  g_total += job->result + t_static_counter;
  pthread_mutex_unlock(&g_lock);
  atomic_fetch_add(&g_done, 1);
  return 0;
}

int
main(int argc, char **argv)
{
  enum { N = 4 };
  pthread_t threads[N];
  Job jobs[N];
  for(int32_t i = 0; i < N; i += 1)
  {
    jobs[i].id = i;
    jobs[i].iterations = 1000 * (argc + i);
    jobs[i].result = 0;
    pthread_create(&threads[i], 0, worker, &jobs[i]);
  }
  for(int32_t i = 0; i < N; i += 1)
  {
    pthread_join(threads[i], 0);
  }
  t_id = 99;
  printf("%s: done=%d total=%lld main_tid=%d\n", argv[0], atomic_load(&g_done), (long long)g_total, t_id);
  return (int)(g_total & 0x7f);
}
