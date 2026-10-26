// The child inherits the parent's exception ports and the breakpoint instructions in its image.
// Modes (argv[1]):
//   (none)   the child runs foo() and exits 0 right away; the parent waits and exits 0
//   "stale"  the child sleeps 300 ms before foo() (long enough for the run which forked it to end
//            and for its breakpoint to be removed), then exits 7; the parent waits, prints what
//            waitpid() returned and exits with the child's exit code
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

volatile int sink;

void foo(void)
{
  sink += 1; // bp_foo
  printf("child: foo ran\n");
  fflush(stdout);
}

int main(int argc, char **argv)
{
  int stale = (argc > 1 && strcmp(argv[1], "stale") == 0);
  pid_t pid = fork();
  if(pid == 0)
  {
    if(stale) { usleep(300000); }
    foo();
    _exit(stale ? 7 : 0);
  }
  sink = 1; // bp_after_fork
  int status = 0;
  pid_t r = waitpid(pid, &status, 0);
  int err = (r < 0) ? errno : 0;
  printf("parent: child status %d\n", status); // bp_after_wait
  printf("parent: waitpid -> %d errno %d status 0x%x exited %d code %d\n", (int)r, err, status, WIFEXITED(status), WIFEXITED(status) ? WEXITSTATUS(status) : -1);
  fflush(stdout);
  return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
