// flows debuggee: signals, exceptions, longjmp.
//
// Modes (argv[1]):
//   "handler"  install a SIGUSR1 handler, raise the signal, exit 0
//   "longjmp"  setjmp in main, longjmp from a callee, exit 0
//   "segv"     write through a null pointer (SIGSEGV)
//   "abort"    abort() (SIGABRT)
//   "trap"     __builtin_trap() (brk)
//   "sigint"   raise(SIGINT); the default action terminates the process
//   "segv_recover"  a SIGSEGV handler siglongjmps out of a null store; prints "recovered", exit 0 (D4)

#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

volatile int g_handler_hits = 0;
volatile int g_after_jump = 0;
static jmp_buf g_env;
int *volatile g_null = 0;

static sigjmp_buf g_recover_env;

static void on_sigsegv(int signo)
{
  g_handler_hits += signo;            // @segv_handler_body
  siglongjmp(g_recover_env, 1);
}

static void on_sigusr1(int signo)
{
  g_handler_hits += 1;                // @handler_body
  g_handler_hits += signo;            // @handler_second
}

__attribute__((noinline)) int raise_usr1(void)
{
  int before = g_handler_hits;
  raise(SIGUSR1);                     // @raise_call
  return g_handler_hits - before;     // @raise_after
}

__attribute__((noinline)) void jumper(int v)
{
  g_after_jump = v;                   // @jumper_body
  longjmp(g_env, v);                  // @longjmp_call
}

__attribute__((noinline)) int do_setjmp(void)
{
  int r = 0;
  if(setjmp(g_env) == 0)              // @setjmp_line
  {
    jumper(5);                        // @jumper_call
    r = -1;                           // never reached
  }
  else
  {
    r = g_after_jump;                 // @after_longjmp
  }
  return r;                           // @setjmp_ret
}

int main(int argc, char **argv)
{
  const char *mode = argc > 1 ? argv[1] : "handler";
  int r = 0;
  if(strcmp(mode, "handler") == 0)
  {
    signal(SIGUSR1, on_sigusr1);
    r = raise_usr1();                 // @main_handler
    printf("handler ran %d\n", r);
    return 0;                         // @handler_exit
  }
  if(strcmp(mode, "longjmp") == 0)
  {
    r = do_setjmp();                  // @main_longjmp
    printf("longjmp %d\n", r);
    return 0;                         // @longjmp_exit
  }
  if(strcmp(mode, "segv") == 0)
  {
    *g_null = 1;                      // @segv_line
    return 0;
  }
  if(strcmp(mode, "abort") == 0)
  {
    abort();                          // @abort_line
  }
  if(strcmp(mode, "trap") == 0)
  {
    __builtin_trap();                 // @trap_line
  }
  if(strcmp(mode, "segv_recover") == 0)
  {
    signal(SIGSEGV, on_sigsegv);
    signal(SIGBUS, on_sigsegv);
    if(sigsetjmp(g_recover_env, 1) == 0)
    {
      *g_null = 1;                    // @recover_fault
    }
    else
    {
      printf("recovered\n");          // @recovered
    }
    return 0;                         // @recover_exit
  }
  if(strcmp(mode, "sigint") == 0)
  {
    raise(SIGINT);                    // @sigint_line
    return 0;                         // @sigint_after
  }
  return 1;
}
