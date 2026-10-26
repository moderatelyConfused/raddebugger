// The child inherits the parent's exception ports and the breakpoint instructions in its image.
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

volatile int sink;

void foo(void)
{
  sink += 1; // bp_foo
  printf("child: foo ran\n");
  fflush(stdout);
}

int main(void)
{
  pid_t pid = fork();
  if(pid == 0)
  {
    foo();
    _exit(0);
  }
  int status = 0;
  waitpid(pid, &status, 0);
  printf("parent: child status %d\n", status); // bp_after_wait
  fflush(stdout);
  return 0;
}
