// flows debuggee: two inlined calls in sequence, with a helper inlined into the inlinee (-O2).
// Scenarios F1/F1b/F2/F3/F7 in run_flows.sh. Exits with (a + b) & 0x7f.

volatile int g_trace = 0;

static inline int helper(int v)
{
  g_trace = v + 100;        // @helper_body
  return v + 1;
}

static inline int next(int v)
{
  g_trace = v * 2;          // @next_first
  int h = helper(v);        // @next_helper_call
  return h + g_trace;       // @next_last
}

int main(int argc, char **argv)
{
  (void)argv;
  int a = next(argc);       // @main_next1
  int b = next(a);          // @main_next2
  g_trace = a + b;          // @main_after
  return g_trace & 0x7f;    // @main_ret
}
