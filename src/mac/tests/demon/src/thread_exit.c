// thread_x is alive at the first stop and gone (exited) before the second one, which a new thread causes.
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

static volatile int sink;

static void *short_lived(void *arg)
{
  (void)arg;
  pthread_setname_np("thread_x");
  usleep(300000);
  return 0;
}

void y_func(void)
{
  sink += 1; // bp_y
}

static void *long_lived(void *arg)
{
  (void)arg;
  pthread_setname_np("thread_y");
  y_func();
  for(;;) { usleep(1000); }
  return 0;
}

int main(void)
{
  pthread_t x, y;
  pthread_create(&x, 0, short_lived, 0);
  usleep(100000);
  sink += 1; // bp_main
  pthread_join(x, 0);
  pthread_create(&y, 0, long_lived, 0);
  pthread_join(y, 0);
  return 0;
}
