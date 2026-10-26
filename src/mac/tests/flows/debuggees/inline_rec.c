// flows debuggee (F4a): a recursive function with one level inlined into its caller g: g's body is
// an inline frame of f (depth 1) whose recursive-call line calls the out-of-line f.
// NOTE: clang refuses to inline a directly self-recursive function into its callers (also with
// always_inline / flatten), so the recursion goes through a volatile function pointer.

volatile int g_rec_trace = 0;
int f(int n);
int (*volatile g_f_ptr)(int) = f;

static inline __attribute__((always_inline)) int f_body(int n)
{
  if(n <= 0) { return 1; }        // @f_base
  int r = g_f_ptr(n - 1);         // @f_rec_call
  g_rec_trace = r;                // @f_after
  return r + n;                   // @f_ret
}

__attribute__((noinline)) int f(int n)
{
  return f_body(n);               // @f_outlined
}

__attribute__((noinline)) int g(int n)
{
  int r = f_body(n);              // @g_call
  g_rec_trace = r + 1;            // @g_after
  return r;                       // @g_ret
}

int main(int argc, char **argv)
{
  (void)argv;
  return g(argc + 3) & 0x7f;      // @rec_main
}
