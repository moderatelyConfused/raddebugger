// Replaces itself with /bin/echo (or with the program named by its first argument).
#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv)
{
  printf("before exec\n");
  fflush(stdout);
  if(argc > 1)
  {
    execl(argv[1], argv[1], (char *)0);
  }
  else
  {
    execl("/bin/echo", "echo", "hi", (char *)0);
  }
  printf("exec failed\n");
  return 1;
}
