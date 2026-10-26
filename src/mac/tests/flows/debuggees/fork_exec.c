// flows debuggee: fork and exec.
//
// Modes (argv[1]):
//   "fork"        fork; the child runs child_work() and exits with 7; the parent waits and exits with the child's code
//   "exec" <exe>  execv(<exe>, {<exe>, "exit"}) (basic exits with 42)
//   "exec_echo"   execl("/bin/echo", "echo", "hi") (D6)

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

volatile int g_fork_marker = 0;

__attribute__((noinline)) int child_work(int v)
{
  g_fork_marker = v;                            // @child_work_body
  return v + 6;
}

int main(int argc, char **argv)
{
  const char *mode = argc > 1 ? argv[1] : "fork";
  if(strcmp(mode, "fork") == 0)
  {
    pid_t pid = fork();                           // @fork_line
    if(pid == 0)
    {
      int code = child_work(1);                   // @child_body
      _exit(code);
    }
    int status = 0;
    waitpid(pid, &status, 0);                     // @parent_wait
    g_fork_marker = 2;                            // @parent_after
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1; // @fork_exit
  }
  if(strcmp(mode, "exec_echo") == 0)
  {
    g_fork_marker = 4;                            // @before_exec_echo
    execl("/bin/echo", "echo", "hi", (char *)0);  // @exec_echo_line
    perror("execl");
    return 100 + errno;                           // the exit code tells the errno (EPERM 1 -> 101, EACCES 13 -> 113)
  }
  if(strcmp(mode, "exec") == 0 && argc > 2)
  {
    char *args[] = { argv[2], "exit", 0 };
    g_fork_marker = 3;                            // @before_exec
    execv(argv[2], args);                         // @exec_line
    perror("execv");
    return 100 + errno;                           // the exit code tells the errno
  }
  return 1;
}
