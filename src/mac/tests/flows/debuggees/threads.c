// flows debuggee: threads.
//
// Modes (argv[1]):
//   "workers"      (default) start 4 workers which each run worker_body(idx), join them, exit 0
//   "spin"         start a spinner thread incrementing g_spin_counter; main loops ~20 s, exit 0
//   "fault"        thread B writes through a null pointer in a loop while thread A spins (D5)
//   "exit_then_bp" thread X returns at once and is joined; then thread Y runs worker_body(7) (D7)
//   "sleeper"      thread Y sleeps 30 s while main loops like "spin" (U3)
//   "barrier_watch" workers A and B wait for one flag; released, A runs into a breakpoint line while
//                  B stores to a watched global (D10: two hits reported from one run)
//   "create_bp"    main creates a thread and hits a breakpoint right after; the thread runs into a
//                  breakpoint on its first line (D11: a hit of a thread whose creation is still queued)

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int g_results[8];
volatile int g_spin_counter = 0;
volatile int g_main_counter = 0;
volatile int g_stop_spin = 0;
int *volatile g_fault_ptr = 0;
volatile int g_fault_loops = 0;

__attribute__((noinline)) int worker_body(int idx)
{
  int v = idx * 10;                   // @worker_body
  g_results[idx] = v;                 // @worker_store
  return v;
}

static void *worker(void *arg)
{
  int idx = (int)(intptr_t)arg;
  worker_body(idx);                   // @worker_call
  return 0;
}

static void *spinner(void *arg)
{
  (void)arg;
  while(!g_stop_spin)
  {
    g_spin_counter += 1;              // @spin_loop
    usleep(100);
  }
  return 0;
}

static void *faulter(void *arg)
{
  (void)arg;
  for(;;)
  {
    g_fault_loops += 1;
    *g_fault_ptr = g_fault_loops;     // @fault_line
  }
  return 0;
}

static void *quick_exit_thread(void *arg)
{
  (void)arg;
  return 0;                           // @quick_exit
}

static void *sleeper(void *arg)
{
  (void)arg;
  sleep(30);                          // @sleeper_sleep
  return 0;
}

volatile int g_barrier = 0;
volatile int g_watched_by_b = 0;
volatile int g_a_count = 0;

static void *barrier_a(void *arg)
{
  (void)arg;
  while(!g_barrier) {}
  g_a_count += 1;                     // @barrier_a_line
  return 0;
}

static void *barrier_b(void *arg)
{
  (void)arg;
  while(!g_barrier) {}
  g_watched_by_b = 1;                 // @barrier_b_store
  return 0;
}

int main(int argc, char **argv)
{
  const char *mode = argc > 1 ? argv[1] : "workers";
  if(strcmp(mode, "fault") == 0)
  {
    pthread_t a, b;
    pthread_create(&a, 0, spinner, 0);
    pthread_create(&b, 0, faulter, 0);
    pthread_join(b, 0);
    g_stop_spin = 1;
    pthread_join(a, 0);
    return 0;
  }
  if(strcmp(mode, "exit_then_bp") == 0)
  {
    pthread_t x, y;
    pthread_create(&x, 0, quick_exit_thread, 0);
    pthread_join(x, 0);                                     // @join_x
    pthread_create(&y, 0, worker, (void *)(intptr_t)7);    // @create_y
    pthread_join(y, 0);
    return 0;
  }
  if(strcmp(mode, "barrier_watch") == 0)
  {
    pthread_t a, b;
    pthread_create(&a, 0, barrier_a, 0);
    pthread_create(&b, 0, barrier_b, 0);
    usleep(20000);
    g_barrier = 1;                                          // @barrier_release
    pthread_join(a, 0);
    pthread_join(b, 0);
    return 0;
  }
  if(strcmp(mode, "create_bp") == 0)
  {
    pthread_t y;
    pthread_create(&y, 0, worker, (void *)(intptr_t)5);    // @create_bp_thread
    g_main_counter += 1;                                    // @after_create
    pthread_join(y, 0);
    return 0;
  }
  if(strcmp(mode, "spin") == 0 || strcmp(mode, "sleeper") == 0)
  {
    pthread_t sp;
    pthread_create(&sp, 0, (strcmp(mode, "spin") == 0) ? spinner : sleeper, 0);
    for(int i = 0; i < 20000 && !g_stop_spin; i += 1)
    {
      g_main_counter += 1;            // @main_spin
      usleep(1000);
    }
    g_stop_spin = 1;
    if(strcmp(mode, "spin") == 0) { pthread_join(sp, 0); }
    return 0;
  }
  pthread_t th[4];
  for(int i = 0; i < 4; i += 1) { pthread_create(&th[i], 0, worker, (void *)(intptr_t)i); } // @create_loop
  for(int i = 0; i < 4; i += 1) { pthread_join(th[i], 0); }                                 // @join_loop
  printf("threads done %d %d %d %d\n", g_results[0], g_results[1], g_results[2], g_results[3]); // @threads_done
  return 0;                                                                                  // @threads_exit
}
