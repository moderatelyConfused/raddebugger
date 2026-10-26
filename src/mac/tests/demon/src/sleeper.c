// A process to attach to: rewrites a counter file every 100ms for 15s (shows that it keeps running).
#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv)
{
  const char *path = argc > 1 ? argv[1] : "sleeper_out.txt";
  for(int i = 0; i < 150; i += 1)
  {
    FILE *f = fopen(path, "w");
    if(f) { fprintf(f, "%d\n", i); fclose(f); }
    usleep(100000);
  }
  return 0;
}
