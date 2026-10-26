// Executable which dlopen()s libshape.dylib at run time (path from argv[1],
// else next to the executable) and resolves its entry points with dlsym.
#include <stdio.h>
#include <string.h>
#include <dlfcn.h>
#include <stdint.h>

typedef int64_t (*AreaFn)(int32_t, int32_t);
typedef int32_t (*CountFn)(void);

static void *
open_shape_lib(const char *exe_path, const char *override)
{
  char path[1024];
  if(override != 0)
  {
    snprintf(path, sizeof(path), "%s", override);
  }
  else
  {
    const char *slash = strrchr(exe_path, '/');
    size_t dir_len = slash ? (size_t)(slash - exe_path + 1) : 0;
    snprintf(path, sizeof(path), "%.*slibshape.dylib", (int)dir_len, exe_path);
  }
  return dlopen(path, RTLD_NOW);
}

int
main(int argc, char **argv)
{
  void *lib = open_shape_lib(argv[0], argc > 1 ? argv[1] : 0);
  if(lib == 0)
  {
    fprintf(stderr, "dlopen failed: %s\n", dlerror());
    return 1;
  }
  AreaFn rect = (AreaFn)dlsym(lib, "shape_rect_area");
  CountFn count = (CountFn)dlsym(lib, "shape_call_count");
  int64_t a = rect ? rect(argc + 3, argc + 4) : -1;
  int32_t c = count ? count() : -1;
  printf("%s: area=%lld count=%d\n", argv[0], (long long)a, c);
  dlclose(lib);
  return (int)(a & 0x7f);
}
