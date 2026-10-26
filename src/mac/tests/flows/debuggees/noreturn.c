// flows debuggee (U2): a_func ends with `bl die` (a noreturn function) and b_func follows it
// immediately in the binary, so the return address of that call is b_func's first instruction.
// die() traps (SIGABRT would do as well, but see the exceptions scenario: soft signals do not stop).

volatile int g_nr = 0;

__attribute__((noinline, noreturn)) void die(void)
{
  g_nr = -1;                          // @die_body
  __builtin_trap();                   // @die_trap
}

__attribute__((noinline)) void a_func(int x)
{
  g_nr = x;                           // @a_body
  die();                              // @a_die
}

__attribute__((noinline)) int b_func(int x)
{
  g_nr = x + 1;                       // @b_body
  return g_nr;
}

int main(int argc, char **argv)
{
  (void)argv;
  if(argc > 5) { return b_func(argc); }
  a_func(argc);                       // @nr_main_call
  return 0;
}
