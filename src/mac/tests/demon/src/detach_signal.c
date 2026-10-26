// Raises a signal and reports whether it reached its handler. Modes (argv[1]):
//   (none)  SIGUSR1 with a handler: "usr1 handled" is printed by the handler, "after" by main, exit 0
//   "term"  SIGTERM with the default disposition: the process dies at the raise; "after" is never printed
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void on_usr1(int signo)
{
  (void)signo;
  const char *msg = "usr1 handled\n";
  write(1, msg, strlen(msg));
}

int main(int argc, char **argv)
{
  int term = (argc > 1 && strcmp(argv[1], "term") == 0);
  signal(SIGUSR1, on_usr1);
  printf("before\n");
  fflush(stdout);
  raise(term ? SIGTERM : SIGUSR1); // raise
  printf("after\n");
  fflush(stdout);
  return 0;
}
