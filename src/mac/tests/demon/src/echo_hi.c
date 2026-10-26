// What exec_echo execs when told to (a binary of our own, instead of the platform's /bin/echo).
#include <stdio.h>

int main(void)
{
  printf("hi\n");
  return 0;
}
