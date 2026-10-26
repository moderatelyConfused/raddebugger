// flows debuggee: a dylib loaded with dlopen by dylib_host.c.

int plugin_counter = 0;

int plugin_fn(int v)
{
  plugin_counter += v;                // @plugin_body
  return plugin_counter * 2;          // @plugin_ret
}
