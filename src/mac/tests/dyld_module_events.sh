#!/bin/zsh
# Proof that dynamic library load/unload events arrive on macOS (src/mac/README.md
# open issue: "could not find _dyld_debugger_notification ... load events will be
# synthesized at stops"; frames in shared-cache modules then had no module / no unwind).
#
# Root cause: the dyld_all_image_infos address captured at launch belongs to the
# initial dyld; on this arm64 macOS two such structs are in flight (the launch one and
# the shared-cache one TASK_DYLD_INFO reports after dyld hands over) and only one is
# populated with the full image array at a given moment, so the per-stop image sync read
# a struct reporting a single image and the notification trap was never placed. The
# demon now reads both candidate structs and keeps whichever lists more images, and arms
# the trap from the pointer dyld publishes (dyld_all_image_infos.notification, the
# shared-cache dyld's _lldb_image_notifier, PAC-stripped).
#
# Checks:
#   - stop in main -> the startup modules libSystem.B.dylib and libsystem_kernel.dylib
#     are reported, both in ctrl_thread.raddbg_log and in `state` modules.
#   - running the dlopen/dlclose loop to its last line -> the plugin produces exactly
#     three LoadModule events (dynamic, via the notification trap) and three
#     UnloadModule events for those module ids, all before the process exits (the demon
#     also unloads every module at exit, so the count is taken while the process is
#     stopped on its last line).
#   - the "synthesized at stops" fallback message is no longer emitted.
#   - a breakpoint on `libsystem_kernel.dylib!write` (a shared-cache libsystem_kernel
#     function; dyld's own syscall stubs and libc++ define `write` too, and an unqualified
#     function breakpoint resolves to whichever module's RDI answers first, so the
#     module-qualified form is used) resolves and hits, and the frames of the shared-cache
#     libraries on the stack are named. Those
#     images have no file of their own on disk and no DWARF: the demon writes a
#     symbol-only copy of each in-memory image into the debugger's cache
#     (<cache>/raddbg/images/<UUID>/<name>) and reports it as the module's debug info,
#     and the converter turns its LC_SYMTAB into RDI procedures (rdi_from_macho.c). The
#     conversion is asynchronous, so the test first waits for `eval write` to resolve
#     (a function breakpoint added before the name is known is never armed: a separate,
#     known engine gap).
#   - a non-exported libsystem_c function (`__vfprintf`, N_PEXT: private to the image,
#     absent from its export trie) resolves and a breakpoint on it hits with printf and
#     main on the stack: the image's own symbol table, which the cache this macOS ships
#     keeps complete, names the locals (a production cache would strip them into a
#     `.symbols` file; none exists here, see the README's Known gaps).
#
# Breakpoints are symbol-based (main, dlhost_done): a freshly built binary's DWARF is
# converted asynchronously on first load, so a source-line breakpoint can be missed before
# this short program runs past it; the Mach-O symbol table is available at once.
#
# The instance runs with HOME under HARNESS_DIR, so the symbol-only image files it writes
# (and the images-cache sweep) stay out of the user's cache.
#
# Settings: RADDBG, IPC_PORT (7609), HARNESS_DIR.
set -u
ROOT="$(cd "$(dirname ${(%):-%x})/../../.." && pwd)"
RD=${RADDBG:-"$ROOT/build/raddbg"}
P=${IPC_PORT:-7609}
# a scratch directory of our own is removed at exit (with the instance's HOME, so the symbol-only
# images it wrote go too); a HARNESS_DIR given by the caller is theirs
if [[ -n ${HARNESS_DIR:-} ]]; then S=$HARNESS_DIR; else S=$(mktemp -d /tmp/raddbg_dyld.XXXXXX); trap 'rm -rf "$S"' EXIT; fi
mkdir -p $S/src $S/home
ipc() { $RD --ipc_port:$P --ipc "$@" 2>&1; }
field() { echo "$1" | grep -m1 " $2:" | awk '{print $2}'; }
state_modules() { ipc state | sed -n '/^ modules:/,/^ }/p' | grep "name:" | sed 's/.*name: *//'; }
# resolved: the reply names a value (an empty or failed reply is not a resolution)
eval_resolved() { local out=$(ipc eval "$1"); [[ -n "$out" ]] && echo "$out" | grep -q " value:" && ! echo "$out" | grep -q "could not be found"; }
wait_resolved() { for i in $(seq 1 100); do eval_resolved "$1" && return 0; sleep 0.1; done; return 1; }
# the symbol-only images of a cold cache are converted asynchronously; a function breakpoint is
# resolved on the control thread at the next run through the name search over every module's
# RDI, with a 5 s budget which a loaded machine exceeds while 45 conversions are in flight (the
# trap is then never placed). Wait until every image directory has its RDI before setting one.
IMAGES=$S/home/Library/Caches/raddbg/images
wait_rdis() { for i in $(seq 1 600); do local d=$(ls $IMAGES 2>/dev/null | wc -l | tr -d ' '); local r=$(ls $IMAGES/*/*.rdi 2>/dev/null | wc -l | tr -d ' '); [[ "$d" -gt 0 && "$r" -ge "$d" ]] && return 0; sleep 0.1; done; echo "NOTE: not every image has an RDI yet ($r of $d)"; return 1; }

# --- build debuggee (plugin + dlopen host) ---
cat > $S/src/plugin.c <<'EOF'
int plugin_value(void){ return 42; }
EOF
cat > $S/src/dlhost.c <<'EOF'
#include <dlfcn.h>
#include <stdio.h>
#include <unistd.h>
__attribute__((noinline)) void dlhost_done(void) { fputs("", stdout); }
int main(void)
{
  write(1, "dlhost start (write)\n", 21); /* stdio's own output goes through write$NOCANCEL, not write */
  printf("dlhost start\n");
  for(int i = 0; i < 3; i += 1)
  {
    void *h = dlopen(PLUGIN_PATH, RTLD_NOW);
    int (*fn)(void) = (int(*)(void))(h ? dlsym(h, "plugin_value") : 0);
    printf("loaded -> %d\n", fn ? fn() : -1);
    usleep(200000);
    if(h) dlclose(h);
    usleep(200000);
  }
  printf("dlhost done\n");
  dlhost_done();
  return 0;
}
EOF
clang -g -O0 -dynamiclib -o $S/src/libplugin.dylib $S/src/plugin.c || { echo "plugin build failed"; exit 1; }
clang -g -O0 -DPLUGIN_PATH="\"$S/src/libplugin.dylib\"" -o $S/src/dlhost $S/src/dlhost.c || { echo "dlhost build failed"; exit 1; }
DL=$S/src

fails=0
INSTANCE_PID=
start_instance() {
  [[ -n "$INSTANCE_PID" ]] && kill -9 $INSTANCE_PID 2>/dev/null
  # a fresh user & project file: `ipc exit` saves the project, and the next instance would
  # otherwise start with the previous one's targets and breakpoints
  rm -f $S/u.raddbg_user $S/p.raddbg; rm -rf $S/logs
  HOME=$S/home $RD --ipc_port:$P --user:$S/u.raddbg_user --project:$S/p.raddbg --logs:$S/logs > $S/out.txt 2>&1 &
  INSTANCE_PID=$!
  for i in $(seq 1 600); do lsof -nP -iTCP:$P -sTCP:LISTEN 2>/dev/null | grep -q raddbg && return 0; sleep 0.05; done
  echo "FAIL: instance did not listen on $P"; fails=$((fails+1)); return 1
}
# exit the instance (flushes its logs), then make sure it is gone
stop_instance() {
  ipc exit >/dev/null 2>&1
  for i in $(seq 1 100); do kill -0 $INSTANCE_PID 2>/dev/null || break; sleep 0.05; done
  kill -9 $INSTANCE_PID 2>/dev/null
  INSTANCE_PID=
}
run_to_stop() {  # $1 = stop_count floor; echoes the new stop_count
  local before=$1
  ipc run >/dev/null 2>&1
  for i in $(seq 1 200); do
    local st=$(ipc state); local sc=$(field "$st" stop_count)
    if [[ "$(field "$st" running)" == "0" && -n "$sc" && "$sc" -gt "$before" ]]; then echo "$sc"; return 0; fi
    sleep 0.1
  done
  echo "$before"; return 1
}
L=$S/logs/ctrl_thread.raddbg_log

echo "=== startup module discovery (stop in main) ==="
start_instance
echo "instance pid $INSTANCE_PID"
ipc add_target "$DL/dlhost" >/dev/null 2>&1
ipc add_function_breakpoint main >/dev/null 2>&1
sc=$(run_to_stop 0)
sym=$(echo "$(ipc state)" | grep -m1 ip_voff_symbol | cut -d'"' -f2)
echo "stopped at main (stop_count=$sc symbol=\"$sym\")"
mods=$(state_modules)
m_count=$(echo "$mods" | grep -c .)
m_kernel=$(echo "$mods" | grep -c 'libsystem_kernel.dylib')
m_libsystem=$(echo "$mods" | grep -c 'libSystem.B.dylib')
echo "state modules: count=$m_count  libSystem.B.dylib=$m_libsystem  libsystem_kernel.dylib=$m_kernel"
[[ "$sym" == main ]]        || { echo "FAIL: did not stop at main"; fails=$((fails+1)); }
[[ "$m_libsystem" -ge 1 && "$m_kernel" -ge 1 ]] || { echo "FAIL: libSystem/libsystem_kernel missing from state modules"; fails=$((fails+1)); }

echo "=== dynamic dlopen/dlclose events (notification trap), counted on the last line ==="
ipc add_function_breakpoint dlhost_done >/dev/null 2>&1
sc=$(run_to_stop $sc)
sym=$(echo "$(ipc state)" | grep -m1 ip_voff_symbol | cut -d'"' -f2)
echo "stopped at dlhost_done (stop_count=$sc symbol=\"$sym\")"
[[ "$sym" == dlhost_done ]] || { echo "FAIL: did not stop at dlhost_done"; fails=$((fails+1)); }
stop_instance   # flushes the ctrl log
# ctrl log: LoadModule events whose string is the plugin path, and UnloadModule events of those module ids
n_libsystem=$(grep -c 'libSystem.B.dylib' $L 2>/dev/null)
n_kernel=$(grep -c 'libsystem_kernel.dylib' $L 2>/dev/null)
n_total_load=$(grep -c 'kind:           LoadModule' $L 2>/dev/null)
read pl_load pl_unload <<< "$(awk -v plugin="\"$DL/libplugin.dylib\"" '
  /dmn_event:/ { kind=""; module=""; str="" }
  /^ *kind: / { kind=$2 }
  /^ *module: / { module=$2 }
  /^ *string: / { str=$2 }
  /^ *ip_vaddr: / { if(kind=="LoadModule" && str==plugin) { loads++; ids[module]=1 } if(kind=="UnloadModule" && (module in ids)) unloads++ }
  END { print loads+0, unloads+0 }' $L)"
echo "ctrl log: LoadModule=$n_total_load  libSystem.B.dylib=$n_libsystem  libsystem_kernel.dylib=$n_kernel  plugin: LoadModule=$pl_load UnloadModule=$pl_unload"
[[ "$n_libsystem" -ge 1 ]]  || { echo "FAIL: no libSystem.B.dylib LoadModule in log"; fails=$((fails+1)); }
[[ "$n_kernel"    -ge 1 ]]  || { echo "FAIL: no libsystem_kernel.dylib LoadModule in log"; fails=$((fails+1)); }
[[ "$pl_load" -eq 3 ]]      || { echo "FAIL: expected 3 plugin LoadModule events, got $pl_load"; fails=$((fails+1)); }
[[ "$pl_unload" -eq 3 ]]    || { echo "FAIL: expected 3 plugin UnloadModule events before exit, got $pl_unload"; fails=$((fails+1)); }
if grep -qi "could not find the dyld debugger notification" $L 2>/dev/null; then
  echo "FAIL: dyld notification function not found"; fails=$((fails+1))
else echo "OK: dyld notification function found (no synthesize-at-stops fallback)"; fi

echo "=== breakpoint on a shared-cache function (write): symbol table -> RDI ==="
start_instance
ipc add_target "$DL/dlhost" >/dev/null 2>&1
ipc add_function_breakpoint main >/dev/null 2>&1
sc=$(run_to_stop 0)
# the symbol-only images are converted asynchronously after the modules load; wait for the RDIs and the name
wait_rdis
WRITE='libsystem_kernel.dylib!write'
wait_resolved "$WRITE" && resolved=1 || resolved=0
echo "eval $WRITE -> $(ipc eval "$WRITE" | grep -m1 'value\|msgs\|could not' | tr -d '\n')"
[[ $resolved -eq 1 ]] || { echo "FAIL: eval $WRITE did not resolve within 10s"; fails=$((fails+1)); }
ipc add_function_breakpoint "$WRITE" >/dev/null 2>&1
sc=$(run_to_stop $sc)
st=$(ipc state)
sym=$(echo "$st" | grep -m1 ip_voff_symbol | cut -d'"' -f2)
mod=$(echo "$st" | grep -m1 ip_module | cut -d'"' -f2)
stack=$(echo "$st" | sed -n '/^ call_stack:/,/^ breakpoints:/p' | grep '   symbol' | cut -d'"' -f2 | tr '\n' ' ')
frames_mods=$(echo "$st" | sed -n '/^ call_stack:/,/^ breakpoints:/p' | grep '   module:' | cut -d'"' -f2 | tr '\n' ' ')
echo "write breakpoint stop: module=\"$mod\" symbol=\"$sym\""
echo "call stack: $stack"
echo "frame modules: $frames_mods"
if [[ "$sym" == write* && "$mod" == *libsystem_kernel* ]]; then echo "OK: write breakpoint hit in libsystem_kernel"
else echo "FAIL: write breakpoint did not hit in libsystem_kernel (module=\"$mod\" symbol=\"$sym\")"; fails=$((fails+1)); fi
# `write` is a frameless syscall stub without a compact unwind entry; stopped at its entry the unwinder
# used to fall back to the frame-pointer chain and skip the caller (stack "write start", no main). The
# symbol-table RDI gives the engine the function's start, and at it lr is the return address (lldb's
# at-function-entry rule), so main is on the stack.
[[ "$stack" == *main* ]] && echo "OK: main on the call stack at write" || { echo "FAIL: main missing from the call stack at write (stack: $stack)"; fails=$((fails+1)); }
# every frame is named: the shared-cache libraries' procedures come from their symbol tables
unnamed=$(echo "$st" | sed -n '/^ call_stack:/,/^ breakpoints:/p' | grep '   symbol' | grep -c '???\|""')
[[ "$unnamed" -eq 0 ]] || { echo "FAIL: $unnamed unnamed frame(s) in the call stack at write"; fails=$((fails+1)); }

echo "=== breakpoint on a non-exported shared-cache function (__vfprintf, libsystem_c) ==="
wait_resolved __vfprintf && resolved=1 || resolved=0
echo "eval __vfprintf -> $(ipc eval __vfprintf | grep -m1 'value\|msgs\|could not' | tr -d '\n')"
[[ $resolved -eq 1 ]] || { echo "FAIL: eval __vfprintf (a private symbol of libsystem_c) did not resolve"; fails=$((fails+1)); }
ipc add_function_breakpoint __vfprintf >/dev/null 2>&1
sc=$(run_to_stop $sc)
st=$(ipc state)
sym=$(echo "$st" | grep -m1 ip_voff_symbol | cut -d'"' -f2)
mod=$(echo "$st" | grep -m1 ip_module | cut -d'"' -f2)
stack=$(echo "$st" | sed -n '/^ call_stack:/,/^ breakpoints:/p' | grep '   symbol' | cut -d'"' -f2 | tr '\n' ' ')
echo "__vfprintf breakpoint stop: module=\"$mod\" symbol=\"$sym\""
echo "call stack: $stack"
if [[ "$sym" == __vfprintf && "$mod" == *libsystem_c* ]]; then echo "OK: __vfprintf breakpoint hit in libsystem_c"
else echo "FAIL: __vfprintf breakpoint did not hit in libsystem_c (module=\"$mod\" symbol=\"$sym\")"; fails=$((fails+1)); fi
[[ "$stack" == *printf* && "$stack" == *main* ]] || { echo "FAIL: printf/main missing from the call stack at __vfprintf"; fails=$((fails+1)); }
unnamed=$(echo "$st" | sed -n '/^ call_stack:/,/^ breakpoints:/p' | grep '   symbol' | grep -c '???\|""')
[[ "$unnamed" -eq 0 ]] || { echo "FAIL: $unnamed unnamed frame(s) in the call stack at __vfprintf"; fails=$((fails+1)); }

stop_instance
pkill -9 -f "^$DL/dlhost" 2>/dev/null
echo "----"
if [[ $fails -eq 0 ]]; then echo "PASS: startup + dynamic module load/unload events arrive (log and state modules); shared-cache symbols, exported and private, resolve"; exit 0
else echo "FAIL: $fails checks failed"; exit 1; fi
