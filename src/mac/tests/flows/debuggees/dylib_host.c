// flows debuggee: dlopen / dlclose a plugin twice. argv[1] is the dylib path.

#include <dlfcn.h>
#include <stdio.h>

int main(int argc, char **argv)
{
  const char *path = argc > 1 ? argv[1] : "libplugin.dylib";
  int r = 0;
  for(int round = 0; round < 2; round += 1)
  {
    void *h = dlopen(path, RTLD_NOW);                       // @dlopen
    if(!h) { fprintf(stderr, "dlopen failed: %s\n", dlerror()); return 3; }
    int (*fn)(int) = (int (*)(int))dlsym(h, "plugin_fn");   // @dlsym
    r += fn(round + 1);                                     // @call_plugin
    dlclose(h);                                             // @dlclose
    r += 1;                                                 // @after_dlclose
  }
  printf("host done %d\n", r);                              // @host_print
  return r;                                                 // @host_exit (the dylib is reloaded fresh: 2+1 + 4+1 = 8)
}
