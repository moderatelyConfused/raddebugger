#!/bin/zsh
# Builds the flow-test debuggees (src/mac/tests/flows/debuggees) into FLOWS_BUILD_DIR
# (default: <repo>/build/flows), each as a -g -O0 and a -g -O2 variant, with dSYM bundles.
# Also builds the Zig generator (GENERATOR_DIR, default ~/Desktop/Etc/October-2026/generator) into
# FLOWS_BUILD_DIR/generator when zig (ZIG, default ~/.zvm/bin/zig) is available (lib.sh's
# generator_locate / generator_build); the generator's own directory is left untouched (its cache
# dir is under FLOWS_BUILD_DIR).
set -e
here=$(cd "$(dirname ${(%):-%x})" && pwd)
repo=$(cd "$here/../../../.." && pwd)
out=${FLOWS_BUILD_DIR:-$repo/build/flows}
src=$here/debuggees
mkdir -p "$out"

cc=${CC:-clang}
cxx=${CXX:-clang++}
common=(-g -fno-omit-frame-pointer -Wall -Wno-unused-function)

# NOTE: compile and link separately: the object file must still exist when dsymutil links
# the dSYM (a one-step `clang -g x.c -o x` deletes its temporary object, leaving only a debug map)
mkdir -p "$out/obj"
build() # <name> <source> <opt flags...>
{
  local name=$1 source=$2; shift 2
  echo "[$name]"
  $cc "${common[@]}" "$@" -c -o "$out/obj/$name.o" "$source"
  $cc "${common[@]}" "$@" -o "$out/$name" "$out/obj/$name.o"
  dsymutil "$out/$name"
}
buildxx() # <name> <source> <opt flags...>
{
  local name=$1 source=$2; shift 2
  echo "[$name]"
  $cxx -std=c++17 "${common[@]}" "$@" -c -o "$out/obj/$name.o" "$source"
  $cxx -std=c++17 "${common[@]}" "$@" -o "$out/$name" "$out/obj/$name.o"
  dsymutil "$out/$name"
}

for variant in O0 O2; do
  opt=-$variant
  build basic_$variant     "$src/basic.c"     $opt
  build signals_$variant   "$src/signals.c"   $opt
  build threads_$variant   "$src/threads.c"   $opt -lpthread
  build dylib_host_$variant "$src/dylib_host.c" $opt
  build fork_exec_$variant "$src/fork_exec.c" $opt
  buildxx cpp_$variant     "$src/cpp.cpp"     $opt
  build coro_$variant      "$src/coro.c"      $opt
  build visit_qsort_$variant "$src/visit_qsort.c" $opt -fblocks
  build inline_flows_$variant "$src/inline_flows.c" $opt
  echo "[libplugin_$variant.dylib]"
  $cc "${common[@]}" $opt -c -o "$out/obj/plugin_$variant.o" "$src/plugin.c"
  $cc "${common[@]}" $opt -dynamiclib -o "$out/libplugin_$variant.dylib" "$out/obj/plugin_$variant.o"
  dsymutil "$out/libplugin_$variant.dylib"
done
# single-variant debuggees of the inline / stepping review scenarios
build inline_rec_O2 "$src/inline_rec.c" -O2
build inl_main_O2   "$src/inl_main.c"   -O2 -I"$src"
build bigframe_O2   "$src/bigframe.c"   -O2
build noreturn_O2   "$src/noreturn.c"   -O2
# large debug info (bp_resolution_converting): 20k functions and struct types, ~19 MB of DWARF,
# so that the debugger converts it for a few seconds at launch; the source is generated here
gen_big_symbols() # <source path> <function count> <members per struct>
{
  local src=$1 n=$2 m=$3 i j
  {
    echo '#include <unistd.h>'
    echo '#include <stdio.h>'
    echo 'volatile int g_sink;'
    for ((i = 1; i <= n; i++)); do
      printf 'typedef struct Big%d { ' $i
      for ((j = 1; j <= m; j++)); do printf 'int a%d_%d; double b%d_%d; ' $i $j $i $j; done
      printf 'struct Big%d *next; char name[%d]; } Big%d;\n' $i $(( (i % 7) + 4 )) $i
      printf '__attribute__((noinline)) int big_fn_%d(Big%d *p, int q) { ' $i $i
      for ((j = 1; j <= m; j++)); do printf 'int local_%d_%d = p->a%d_%d + q + %d; ' $i $j $i $j $j; done
      printf 'Big%d tmp = *p; tmp.a%d_1 += local_%d_1; g_sink = tmp.a%d_1' $i $i $i $i
      for ((j = 2; j <= m; j++)); do printf ' + local_%d_%d' $i $j; done
      printf '; return tmp.a%d_1 + (int)tmp.b%d_1; }\n' $i $i
    done
    echo 'int main(int argc, char **argv) {'
    echo '  int spin = argc > 1;'
    echo '  Big1 b1 = {0};'
    echo '  for(int i = 0; i < (spin ? 30000 : 3); i += 1) { g_sink += big_fn_1(&b1, i); usleep(1000); } // @big_loop'
    printf '%s\n' '  printf("big done %d\n", g_sink);'
    echo '  return 0;'
    echo '}'
  } > "$src"
}
echo "[big_symbols.c (generated)]"
gen_big_symbols "$out/big_symbols.c" 20000 6
build big_symbols "$out/big_symbols.c" -O0
build stackswitch_O2 "$src/stackswitch.c" -O2
# -O2 without frame pointers (call stack through compact-unwind / eh_frame only)
echo "[basic_O2_nofp]"
$cc -g -O2 -fomit-frame-pointer -Wall -c -o "$out/obj/basic_O2_nofp.o" "$src/basic.c"
$cc -g -O2 -fomit-frame-pointer -Wall -o "$out/basic_O2_nofp" "$out/obj/basic_O2_nofp.o"
dsymutil "$out/basic_O2_nofp"

# Zig generator (debug build), see README "Status and open issues"; located and built by lib.sh
# (ZIG, GENERATOR_DIR), skipped when either is missing. (lib.sh wants a HARNESS_DIR; a build has no
# instance, so the output directory stands in for it rather than a temp dir of its own.)
export HARNESS_DIR=${HARNESS_DIR:-$out}
source "$here/../lib.sh"
if generator_locate; then
  echo "[generator (zig)]"
  generator_build "$out/generator" || echo "zig build failed (generator scenarios will be skipped)"
else
  echo "zig or the generator project not found; generator scenarios will be skipped"
fi
echo "debuggees built into $out"
