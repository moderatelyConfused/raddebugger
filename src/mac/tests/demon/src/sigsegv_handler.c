// A SIGSEGV handler which recovers with siglongjmp: it must run when the debugger passes the exception.
#include <stdio.h>
#include <signal.h>
#include <setjmp.h>

static sigjmp_buf jb;

static void on_segv(int signo)
{
  siglongjmp(jb, signo);
}

int main(void)
{
  struct sigaction sa = {0};
  sa.sa_handler = on_segv;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGSEGV, &sa, 0);
  sigaction(SIGBUS, &sa, 0);
  if(sigsetjmp(jb, 1) == 0)
  {
    *(volatile int *)0 = 1; // fault
    printf("not reached\n");
  }
  else
  {
    printf("recovered\n");
  }
  fflush(stdout);
  return 0;
}
