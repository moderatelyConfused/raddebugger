// 32 threads call work() in a loop: a breakpoint in work() is hit by several threads at once, over and
// over (stress for the demon's stop rounds: hits arriving during internal steps, held-back hits).
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>

static _Atomic long g_work;

__attribute__((noinline)) void work(void)
{
  g_work += 1; // bp_work
}

static void *worker(void *arg)
{
  (void)arg;
  for(;;)
  {
    work();
    usleep(200);
  }
  return 0;
}

int main(void)
{
  pthread_t th[32];
  for(int i = 0; i < 32; i += 1) { pthread_create(&th[i], 0, worker, 0); }
  for(;;) { usleep(100000); }
  printf("%ld\n", (long)g_work);
  return 0;
}
