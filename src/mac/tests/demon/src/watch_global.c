// Writes a global in a loop (target of a data breakpoint). The breakpoint's address expression is the
// local `p`: `&g_counter` itself evaluates to 0 for these binaries (the global's address is missing
// from the converted debug info), so the address goes through a local, read at the second run.
#include <stdio.h>
#include <unistd.h>

int g_counter;

int main(void)
{
  int *p = &g_counter;
  int i = 0; // bp_setup
  for(i = 0; i < 50; i += 1)
  {
    g_counter += 1; // write
    usleep(10000);
  }
  printf("done %d %p\n", g_counter, (void *)p);
  return 0;
}
