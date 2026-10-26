// flows debuggee: basic control flow, stepping, evaluation and watchpoint targets.
// Lines the runner refers to carry an `// @tag` comment; the runner looks the line numbers up by tag.
//
// Modes (argv[1]):
//   (none) / "exit"  run everything, exit with code 42
//   "spin"           loop for up to ~30 s (attach / halt / detach tests), exit 0

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct Point { int x; int y; } Point;
typedef struct Record Record;
struct Record { const char *name; Point pt; int values[4]; double weight; Record *next; };

int g_counter = 0;                    // global
static int s_static_counter = 100;    // static
__thread int t_tls_counter = 7;       // thread-local
const char *g_message = "hello flows";
Record g_record = { "rec", {3, 4}, {1, 2, 3, 4}, 2.5, 0 };
int g_array[8] = {0, 1, 2, 3, 4, 5, 6, 7};
volatile int g_watched = 0;           // watchpoint target
volatile int g_sink = 0;
volatile int g_inline_trace = 0;

// inlined at -O2, real calls at -O0; the volatile stores keep instructions of
// the inlined bodies' own lines alive, so -O2 emits inline sites for them
static inline int square(int v)
{
  int r = v * v;
  g_inline_trace = r;                 // @inline_body
  return r;
}

static inline int square_sum(int a, int b)
{
  int s = square(a) + square(b);      // @inline_outer_call
  g_inline_trace = s;                 // @inline_outer_body
  return s;
}

__attribute__((noinline)) int inner(int a)
{
  int r = a + 1;                      // @inner_body
  return r;
}

__attribute__((noinline)) int middle(int a)
{
  int r = inner(a * 2);               // @middle_call
  return r + 1;                       // @middle_ret
}

__attribute__((noinline)) int outer(int a)
{
  int r = middle(a + 3);              // @outer_call
  g_counter += r;                     // @outer_after
  return r;                           // @outer_ret
}

__attribute__((noinline)) int factorial(int n)
{
  if(n <= 1) { return 1; }            // @fact_base
  int r = n * factorial(n - 1);       // @fact_rec
  return r;                           // @fact_ret
}

__attribute__((noinline)) int tail_target(int v)
{
  int r = v + 100;                    // @tail_target_body
  return r;
}

__attribute__((noinline)) int tail_caller(int v)
{
  g_counter += v;                     // @tail_caller_body
  return tail_target(v);              // @tail_call  (-O2: `b tail_target`)
}

__attribute__((noinline)) int loop_sum(int n)
{
  int sum = 0;
  for(int i = 0; i < n; i += 1)
  {
    sum += i;                         // @loop_body
    g_watched = sum;                  // @watch_write
  }
  return sum;                         // @loop_after
}

__attribute__((noinline)) int one_line_loop(int n)
{
  int acc = 0;
  for(int i = 0; i < n; i += 1) { acc += square_sum(i, i + 1); g_sink = acc; } // @one_line_loop
  return acc;                         // @one_line_after
}

__attribute__((noinline)) size_t libsystem_call(const char *s)
{
  size_t len = strlen(s);             // @libsystem_call
  return len;                         // @libsystem_after
}

__attribute__((noinline)) int locals_frame(int a, int b)
{
  Point pt = { a, b };                // @locals_first
  int arr[3] = { a, b, a + b };
  const char *str = "frame-string";
  Record *rec = &g_record;
  int r = inner(a);                   // @locals_call
  return r + pt.x + arr[2] + (int)strlen(str) + rec->pt.y; // @locals_after
}

int main(int argc, char **argv)
{
  const char *mode = argc > 1 ? argv[1] : "exit";
  if(strcmp(mode, "spin") == 0)
  {
    for(int i = 0; i < 30000; i += 1)
    {
      g_counter += 1;                 // @spin_body
      usleep(1000);
    }
    return 0;
  }
  int n = argc + 4;                   // 5 when run without arguments beyond the mode
  int r = 0;
  r += outer(argc);                   // @main_outer
  r += factorial(n);                  // @main_fact
  r += tail_caller(n);                // @main_tail
  r += loop_sum(n);                   // @main_loop
  r += one_line_loop(n - 1);          // @main_one_line
  r += (int)libsystem_call(g_message);// @main_libsystem
  r += locals_frame(10, 20);          // @main_locals
  t_tls_counter += 1;                 // @main_tls
  s_static_counter += r;              // @main_static
  printf("basic done %d\n", r);       // @main_print
  return 42;                          // @main_exit
}
