// Three threads raise at once: one faults (its exception is reported and stays pending while it is
// frozen), one hits a breakpoint (the thread the script steps), one raises a signal the engine passes
// through (so that the step's run carries "deliver, do not ignore").
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>

static _Atomic int go;
static volatile int sink;

static void on_usr1(int signo) { (void)signo; }

static void *fault_thread(void *arg)
{
  (void)arg;
  pthread_setname_np("fault_thread");
  while(!go) {}
  for(;;) { *(volatile int *)0 = 1; } // fault
  return 0;
}

void bp_func(void)
{
  sink += 1; // bp_func
}

static void *bp_thread(void *arg)
{
  (void)arg;
  pthread_setname_np("bp_thread");
  while(!go) {}
  bp_func();
  for(;;) { usleep(1000); }
  return 0;
}

static void *signal_thread(void *arg)
{
  (void)arg;
  pthread_setname_np("signal_thread");
  while(!go) {}
  pthread_kill(pthread_self(), SIGUSR1);
  for(;;) { usleep(1000); }
  return 0;
}

int main(void)
{
  signal(SIGUSR1, on_usr1);
  pthread_t a, b, c;
  pthread_create(&a, 0, fault_thread, 0);
  pthread_create(&b, 0, bp_thread, 0);
  pthread_create(&c, 0, signal_thread, 0);
  usleep(100000);
  go = 1;
  for(;;) { usleep(1000); }
  return 0;
}
