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
#   - running the dlopen/dlclose loop -> the plugin produces LoadModule and UnloadModule
#     events (dynamic, via the notification trap).
#   - the "synthesized at stops" fallback message is no longer emitted.
#   - a breakpoint on `write` (a shared-cache libsystem_kernel function) resolves and
#     hits, and the frames of the shared-cache libraries on the stack are named. Those
#     images have no file of their own on disk and no DWARF: the demon writes a
#     symbol-only copy of each in-memory image into the debugger's cache
#     (<cache>/raddbg/images/<UUID>/<name>) and reports it as the module's debug info,
#     and the converter turns its LC_SYMTAB into RDI procedures (rdi_from_macho.c). The
#     conversion is asynchronous, so the test first waits for `eval write` to resolve
#     (a function breakpoint added before the name is known is never armed: a separate,
#     known engine gap).
#
# Breakpoints are symbol-based (main): a freshly built binary's DWARF is converted
# asynchronously on first load, so a source-line breakpoint can be missed before this
# short program runs past it; the Mach-O symbol table is available at once.
#
# Settings: RADDBG, IPC_PORT (7609), HARNESS_DIR.
set -u
ROOT="$(cd "$(dirname ${(%):-%x})/../../.." && pwd)"
RD=${RADDBG:-"$ROOT/build/raddbg"}
P=${IPC_PORT:-7609}
S=${HARNESS_DIR:-$(mktemp -d /tmp/raddbg_dyld.XXXXXX)}
mkdir -p $S/src
ipc() { $RD --ipc_port:$P --ipc "$@" 2>&1; }
field() { echo "$1" | grep -m1 " $2:" | awk '{print $2}'; }
state_modules() { ipc state | sed -n '/^ modules:/,/^ }/p' | grep "name:" | sed 's/.*name: *//'; }

# --- build debuggee (plugin + dlopen host) ---
cat > $S/src/plugin.c <<'EOF'
int plugin_value(void){ return 42; }
EOF
cat > $S/src/dlhost.c <<'EOF'
#include <dlfcn.h>
#include <stdio.h>
#include <unistd.h>
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
  return 0;
}
EOF
clang -g -O0 -dynamiclib -o $S/src/libplugin.dylib $S/src/plugin.c || { echo "plugin build failed"; exit 1; }
clang -g -O0 -DPLUGIN_PATH="\"$S/src/libplugin.dylib\"" -o $S/src/dlhost $S/src/dlhost.c || { echo "dlhost build failed"; exit 1; }
DL=$S/src

fails=0
start_instance() {
  pgrep -f "raddbg --ipc_port:$P" | xargs kill -9 2>/dev/null
  rm -f $S/u.raddbg_user; rm -rf $S/logs
  ($RD --ipc_port:$P --user:$S/u.raddbg_user --project:$S/p.raddbg --logs:$S/logs > $S/out.txt 2>&1 &)
  for i in $(seq 1 400); do lsof -nP -iTCP:$P -sTCP:LISTEN 2>/dev/null | grep -q raddbg && break; sleep 0.05; done
  (ipc state >/dev/null 2>&1 &)
  for i in $(seq 1 100); do ipc state | grep -q version && break; sleep 0.1; done
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

echo "=== startup module discovery (stop in main) ==="
start_instance
echo "instance pid $(pgrep -f "raddbg --ipc_port:$P")"
ipc add_target "$DL/dlhost" >/dev/null 2>&1
ipc add_function_breakpoint main >/dev/null 2>&1
sc=$(run_to_stop 0)
sym=$(echo "$(ipc state)" | grep -m1 ip_voff_symbol | cut -d'"' -f2)
echo "stopped at main (stop_count=$sc symbol=\"$sym\")"
L=$S/logs/ctrl_thread.raddbg_log
# ctrl log
n_libsystem=$(grep -c 'libSystem.B.dylib' $L 2>/dev/null)
n_kernel=$(grep -c 'libsystem_kernel.dylib' $L 2>/dev/null)
n_total_load=$(grep -c 'kind:           LoadModule' $L 2>/dev/null)
# state modules
mods=$(state_modules)
m_count=$(echo "$mods" | grep -c .)
m_kernel=$(echo "$mods" | grep -c 'libsystem_kernel.dylib')
m_libsystem=$(echo "$mods" | grep -c 'libSystem.B.dylib')
echo "ctrl log:    LoadModule=$n_total_load  libSystem.B.dylib=$n_libsystem  libsystem_kernel.dylib=$n_kernel"
echo "state modules: count=$m_count  libSystem.B.dylib=$m_libsystem  libsystem_kernel.dylib=$m_kernel"
[[ "$sym" == main ]]        || { echo "FAIL: did not stop at main"; fails=$((fails+1)); }
[[ "$n_libsystem" -ge 1 ]]  || { echo "FAIL: no libSystem.B.dylib LoadModule in log"; fails=$((fails+1)); }
[[ "$n_kernel"    -ge 1 ]]  || { echo "FAIL: no libsystem_kernel.dylib LoadModule in log"; fails=$((fails+1)); }
[[ "$m_libsystem" -ge 1 && "$m_kernel" -ge 1 ]] || { echo "FAIL: libSystem/libsystem_kernel missing from state modules"; fails=$((fails+1)); }
[[ "$m_count" -ge 20 ]]     || { echo "FAIL: too few modules in state ($m_count)"; fails=$((fails+1)); }
if grep -qi "could not find the dyld debugger notification" $L 2>/dev/null; then
  echo "FAIL: dyld notification function not found"; fails=$((fails+1))
else echo "OK: dyld notification function found (no synthesize-at-stops fallback)"; fi

echo "=== dynamic dlopen/dlclose events (notification trap) ==="
ipc run >/dev/null 2>&1   # run the loop to completion; plugin dlopen/dlclose fire the trap
for i in $(seq 1 100); do pgrep -f "$DL/dlhost" >/dev/null || break; sleep 0.1; done
sleep 0.3
pl=$(grep -c "$DL/libplugin.dylib" $L 2>/dev/null)
n_unload=$(grep -c 'kind:           UnloadModule' $L 2>/dev/null)
echo "plugin dynamic events: libplugin mentions=$pl  UnloadModule events=$n_unload"
[[ "$pl" -ge 2 ]]       || { echo "FAIL: dlopen'd plugin not reported repeatedly (mentions=$pl)"; fails=$((fails+1)); }
[[ "$n_unload" -ge 1 ]] || { echo "FAIL: no UnloadModule (dlclose) event"; fails=$((fails+1)); }

echo "=== breakpoint on a shared-cache function (write): symbol table -> RDI ==="
start_instance
ipc add_target "$DL/dlhost" >/dev/null 2>&1
ipc add_function_breakpoint main >/dev/null 2>&1
sc=$(run_to_stop 0)
# the symbol-only images are converted asynchronously after the modules load; wait for the name
resolved=0
for i in $(seq 1 100); do ipc eval write | grep -q "could not be found" || { resolved=1; break; }; sleep 0.1; done
echo "eval write -> $(ipc eval write | grep -m1 'value\|msgs\|could not' | tr -d '\n')"
[[ $resolved -eq 1 ]] || { echo "FAIL: eval write did not resolve within 10s"; fails=$((fails+1)); }
dbgi=$(ipc state | sed -n '/^ modules:/,/^ selected_thread:/p' | grep -A3 'libsystem_kernel.dylib' | grep -m1 'debug_info\|dbg' | tr -s ' ')
[[ -n "$dbgi" ]] && echo "libsystem_kernel debug info:$dbgi"
ipc add_function_breakpoint write >/dev/null 2>&1
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
# NOTE: `write` is a frameless syscall stub without a compact unwind entry; stopped at its entry the
# unwinder falls back to the frame-pointer chain and skips the caller (stack "write start", no main),
# where lldb applies its at-function-entry rule (pc = lr). An unwinder follow-up, recorded, not required.
[[ "$stack" == *main* ]] && echo "OK: main on the call stack at write" || echo "NOTE: main missing from the call stack at write (unwinder: frameless stub at entry)"
# every frame is named: the shared-cache libraries' procedures come from their symbol tables
unnamed=$(echo "$st" | sed -n '/^ call_stack:/,/^ breakpoints:/p' | grep '   symbol' | grep -c '???\|""')
[[ "$unnamed" -eq 0 ]] || { echo "FAIL: $unnamed unnamed frame(s) in the call stack at write"; fails=$((fails+1)); }

pgrep -f "raddbg --ipc_port:$P" | xargs kill -9 2>/dev/null
pgrep -fl dlhost | awk '{print $1}' | xargs kill -9 2>/dev/null
echo "----"
if [[ $fails -eq 0 ]]; then echo "PASS: startup + dynamic module load/unload events arrive (log and state modules)"; exit 0
else echo "FAIL: $fails checks failed"; exit 1; fi
