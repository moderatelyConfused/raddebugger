// Signal handlers: sigaction with SA_SIGINFO for SIGUSR1 and SIGALRM, a
// SIGSEGV handler on an alternate stack which recovers with siglongjmp.
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <setjmp.h>

static volatile sig_atomic_t g_usr1_count = 0;
static volatile sig_atomic_t g_alrm_count = 0;
static volatile sig_atomic_t g_segv_count = 0;
static sigjmp_buf g_recover;
static uint8_t g_altstack[1 << 16];

static void
on_usr1(int signo, siginfo_t *info, void *uctx)
{
  (void)uctx;
  g_usr1_count += 1 + (info->si_signo == signo ? 0 : 100);
}

static void
on_alrm(int signo, siginfo_t *info, void *uctx)
{
  (void)info; (void)uctx; (void)signo;
  g_alrm_count += 1;
}

static void
on_segv(int signo, siginfo_t *info, void *uctx)
{
  (void)uctx; (void)signo;
  g_segv_count += 1;
  if(info->si_addr == 0 || g_segv_count > 3)
  {
    siglongjmp(g_recover, 1);
  }
  siglongjmp(g_recover, 2);
}

static void
install(int signo, void (*fn)(int, siginfo_t *, void *), int extra_flags)
{
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = fn;
  sa.sa_flags = SA_SIGINFO | extra_flags;
  sigemptyset(&sa.sa_mask);
  sigaction(signo, &sa, 0);
}

__attribute__((noinline)) static int
touch_bad_address(int argc)
{
  volatile int32_t *p = (volatile int32_t *)(uintptr_t)(argc > 100 ? 0x10 : 0x8);
  return *p; // faults
}

int
main(int argc, char **argv)
{
  stack_t ss;
  ss.ss_sp = g_altstack;
  ss.ss_size = sizeof(g_altstack);
  ss.ss_flags = 0;
  sigaltstack(&ss, 0);

  install(SIGUSR1, on_usr1, 0);
  install(SIGALRM, on_alrm, 0);
  install(SIGSEGV, on_segv, SA_ONSTACK);

  for(int i = 0; i < argc + 1; i += 1)
  {
    raise(SIGUSR1);
  }
  alarm(0);
  raise(SIGALRM);

  int recovered = sigsetjmp(g_recover, 1);
  if(recovered == 0)
  {
    touch_bad_address(argc);
  }

  printf("%s: usr1=%d alrm=%d segv=%d recovered=%d\n", argv[0],
         (int)g_usr1_count, (int)g_alrm_count, (int)g_segv_count, recovered);
  return (g_usr1_count + g_alrm_count + g_segv_count) & 0x7f;
}
