#!/bin/zsh
# Shared setup for the macOS demon checks (src/mac/tests/demon/*.sh). Each check is a zsh script which
# sources this file, builds a small C debuggee, writes a project file, drives a private debugger
# instance through the IPC harness (src/mac/tests/ipc_harness.sh) and inspects the result through
# `raddbg --ipc state`, the debuggee's stdout file and the control thread's log.
# Settings: IPC_PORT (default 7950), HARNESS_DIR (default a temp dir), RADDBG (default build/raddbg).
# Never run raddbg without an --ipc_port: the user's own debugger listens on the default port.
export IPC_PORT=${IPC_PORT:-7950}
export HARNESS_DIR=${HARNESS_DIR:-$(mktemp -d /tmp/raddbg_demon_tests.XXXXXX)}
DEMON_TESTS_DIR="$(cd "$(dirname ${(%):-%x})" && pwd)"
REPO_ROOT="$(cd "$DEMON_TESTS_DIR/../../../.." && pwd)"
source "$DEMON_TESTS_DIR/../ipc_harness.sh"
S=$HARNESS_DIR
CTRL_LOG=$S/logs/ctrl_thread.raddbg_log
FAILS=0

# rjf: build a debuggee from src/<name>.c into $S/<name>. compiled and linked in two steps so that the
# object file (which holds the DWARF; the executable only has a debug map pointing at it) survives,
# then linked into a dSYM, which the debugger's converter reads directly.
build_c() { clang -g -O0 ${=CFLAGS_EXTRA} -c -o "$S/$1.o" "$DEMON_TESTS_DIR/src/$1.c" && clang -o "$S/$1" "$S/$1.o" && dsymutil "$S/$1" >/dev/null 2>&1; }
# rjf: line number of the first line of src/<name>.c containing <marker>
line_of() { grep -n -m1 -- "$2" "$DEMON_TESTS_DIR/src/$1.c" | cut -d: -f1; }
# rjf: write a project file for executable $2 (arguments $3) to $S/$1.raddbg; extra top-level lines come
# from stdin. (the version header line is required: a file without it is treated as a new, empty project)
write_project() {
  local name=$1 exe=$2 args=$3
  { printf '// raddbg 0.9.30 project file\n\ntarget:\n{\n  executable: "%s"\n  arguments: "%s"\n  working_directory: "%s"\n  stdout_path: "%s"\n  enabled: 1\n}\n' "$exe" "$args" "$S" "$S/$name.stdout.txt"; cat; } > "$S/$name.raddbg"
  rm -f "$S/$name.stdout.txt"
}
# rjf: thread count & names from a `state` dump
thread_count() { echo "$1" | sed -n '/^ threads:/,/^ }/p' | grep -c "   id:"; }
thread_names() { echo "$1" | sed -n '/^ threads:/,/^ }/p' | grep "name:" | cut -d'"' -f2; }
thread_ids() { echo "$1" | sed -n '/^ threads:/,/^ }/p' | grep "   id:" | awk '{print $2}'; }
module_names() { echo "$1" | sed -n '/^ modules:/,/^ }/p' | grep "name:" | cut -d'"' -f2; }
# rjf: poll `state` (every 100ms, up to $1 x 100ms) until the thread list is empty
wait_no_threads() { for i in $(seq 1 ${1:-30}); do local st=$(ipc state); [[ "$(thread_count "$st")" == "0" && "$(field "$st" running)" == "0" ]] && return 0; sleep 0.1; done; return 1; }
# rjf: poll until a process named $1 (full path) is gone
wait_process_gone() { for i in $(seq 1 ${2:-50}); do pgrep -f "^$1" >/dev/null || return 0; sleep 0.1; done; return 1; }
# rjf: demon events of a kind in the control thread log (count)
log_events() { grep -E "kind: +$1\$" "$CTRL_LOG" 2>/dev/null | wc -l | tr -d ' '; }
# rjf: record a check
check() { if eval "$2"; then echo "  PASS  $1"; else echo "  FAIL  $1"; FAILS=$((FAILS+1)); fi; }
# rjf: end a check script: stop the debugger instance and any debuggee it launched
finish() { kill_test_instances; pkill -9 -f "^$S/" 2>/dev/null; if [[ $FAILS == 0 ]]; then echo "$1: all checks passed"; else echo "$1: $FAILS check(s) FAILED"; fi; return $FAILS; }
