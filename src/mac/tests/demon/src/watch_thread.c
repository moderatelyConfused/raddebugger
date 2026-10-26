// A thread created after the data breakpoint was armed writes the watched global. The breakpoint's
// address expression is the local `p` (see watch_global.c for why). The writer waits until main has
// raised a signal the engine passes (SIGUSR1, its stop turned off in the project): the demon learns
// of a new thread only at a stop (Mach has no thread-creation notification), and the signal's round
// is where it meets the writer and programs the watchpoint into it.
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>

int g_counter;
static _Atomic int go;

static void on_usr1(int signo) { (void)signo; }

static void *writer(void *arg)
{
  (void)arg;
  pthread_setname_np("writer");
  while(!go) { usleep(1000); }
  for(int i = 0; i < 50; i += 1)
  {
    g_counter += 1; // write
    usleep(10000);
  }
  return 0;
}

int main(void)
{
  signal(SIGUSR1, on_usr1);
  int *p = &g_counter;
  int i = 0; // bp_setup
  pthread_t t;
  pthread_create(&t, 0, writer, 0); // create
  raise(SIGUSR1);                   // the stop which introduces the writer to the demon
  go = 1;
  pthread_join(t, 0);
  printf("done %d %p %d\n", g_counter, (void *)p, i);
  return 0;
}
