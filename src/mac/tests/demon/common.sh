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
mkdir -p "$S"
CTRL_LOG=$S/logs/ctrl_thread.raddbg_log
FAILS=0
# rjf: pids of processes this script started itself (register with `track_pid`); finish kills them
typeset -a TEST_PIDS
track_pid() { TEST_PIDS+=($1); }

# rjf: build a debuggee from src/<name>.c into $S/<name>. compiled and linked in two steps so that the
# object file (which holds the DWARF; the executable only has a debug map pointing at it) survives,
# then linked into a dSYM, which the debugger's converter reads directly.
build_c() { clang -g -O0 ${=CFLAGS_EXTRA} -c -o "$S/$1.o" "$DEMON_TESTS_DIR/src/$1.c" && clang -o "$S/$1" "$S/$1.o" ${=LDFLAGS_EXTRA} && dsymutil "$S/$1" >/dev/null 2>&1; }
# rjf: line number of the first line of src/<name>.c containing <marker>
line_of() { grep -n -m1 -- "$2" "$DEMON_TESTS_DIR/src/$1.c" | cut -d: -f1; }
# rjf: write a project file for executable $2 (arguments $3) to $S/$1.raddbg; extra top-level lines come
# from stdin. (the version header line is required: a file without it is treated as a new, empty project)
write_project() {
  local name=$1 exe=$2 args=$3
  { printf '// raddbg 0.9.30 project file\n\ntarget:\n{\n  executable: "%s"\n  arguments: "%s"\n  working_directory: "%s"\n  stdout_path: "%s"\n  enabled: 1\n}\n' "$exe" "$args" "$S" "$S/$name.stdout.txt"; cat; } > "$S/$name.raddbg"
  rm -f "$S/$name.stdout.txt"
}
# rjf: section-bounded readers of a `state` dump (the dump lists the stop's lines, then the threads,
# modules, processes, call stack and breakpoints; a whole-dump grep for a name or a line number also
# matches thread names, breakpoint locations and call stack entries)
thread_count()  { echo "$1" | sed -n '/^ threads:/,/^ }/p' | grep -c "   id:"; }
thread_names()  { echo "$1" | sed -n '/^ threads:/,/^ }/p' | grep "name:" | cut -d'"' -f2; }
thread_ids()    { echo "$1" | sed -n '/^ threads:/,/^ }/p' | grep "   id:" | awk '{print $2}'; }
module_names()  { echo "$1" | sed -n '/^ modules:/,/^ }/p' | grep "name:" | cut -d'"' -f2; }
process_ids()   { echo "$1" | sed -n '/^ processes:/,/^ }/p' | grep "   id:" | awk '{print $2}'; }
stop_symbol()   { echo "$1" | grep -m1 "ip_voff_symbol:" | cut -d'"' -f2; }
stop_explanation() { echo "$1" | grep -m1 "explanation:" | cut -d'"' -f2; }
# rjf: the stop's innermost source line, "file.c:N" (empty when the stop has no line)
stop_file_line() { echo "$1" | sed -n '/^ lines:/,/^ threads:/p' | grep "file_name\|line_num" | paste - - | awk -F'"' '{split($3,a,":"); gsub(/ /,"",a[2]); printf "%s:%s\n", $2, a[2]}' | sed 's#^.*/##' | tail -1; }
stop_line()     { stop_file_line "$1" | cut -d: -f2; }
# rjf: poll `state` (every 100ms, up to $1 x 100ms) until the thread list is empty
wait_no_threads() { for i in $(seq 1 ${1:-30}); do local st=$(ipc state); [[ "$(thread_count "$st")" == "0" && "$(field "$st" running)" == "0" ]] && return 0; sleep 0.1; done; return 1; }
# rjf: poll until a process named $1 (full path) is gone
wait_process_gone() { for i in $(seq 1 ${2:-50}); do pgrep -f "^$1" >/dev/null || return 0; sleep 0.1; done; return 1; }
# rjf: poll until pid $1 is gone (a zombie counts as gone)
wait_pid_gone() { for i in $(seq 1 ${2:-50}); do [[ "$(ps -o stat= -p $1 2>/dev/null | tr -d ' ')" == "" || "$(ps -o stat= -p $1 2>/dev/null | tr -d ' ')" == Z* ]] && return 0; sleep 0.1; done; return 1; }
# rjf: demon events of a kind in the control thread log (count); log_events_before <kind> <marker>: those before the first line matching <marker>
log_events() { grep -E "kind: +$1\$" "$CTRL_LOG" 2>/dev/null | wc -l | tr -d ' '; }
log_events_before() { sed -n "1,/$2/p" "$CTRL_LOG" 2>/dev/null | grep -E "kind: +$1\$" | wc -l | tr -d ' '; }
# rjf: the control thread's log is flushed a little after the state it describes is visible over IPC:
# wait_log <grep -E pattern> [tries x 100ms] polls for a line before a check reads the log
wait_log() { for i in $(seq 1 ${2:-30}); do grep -qE "$1" "$CTRL_LOG" 2>/dev/null && return 0; sleep 0.1; done; return 1; }
# rjf: record a check. `check <description> <zsh expression>`: the expression is evaluated in this shell,
# so it should be single-quoted and refer to variables by name (`[[ $(stop_symbol "$st") == main ]]`),
# never embed a dump in itself (a quote in a path or thread name would break it). a failed evaluation
# (a syntax error, say) counts as a failure and prints zsh's message.
check() { if eval "$2" 2>$S/check_err.txt; then echo "  PASS  $1"; else echo "  FAIL  $1"; [[ -s $S/check_err.txt ]] && sed 's/^/        /' $S/check_err.txt; FAILS=$((FAILS+1)); fi; }
# rjf: `check_eq <description> <expected> <observed>`
check_eq() { if [[ "$2" == "$3" ]]; then echo "  PASS  $1"; else echo "  FAIL  $1: expected <$2>, observed <$3>"; FAILS=$((FAILS+1)); fi; }
# rjf: end a check script: stop the debuggees the instance still holds (kill_all), the instance, and the
# processes the script started (track_pid). debuggees detached meanwhile keep running by design; they
# are built into $S, so as a last resort the processes started from $S are killed, when $S is a real
# directory of our own (never an empty or root path).
finish() {
  local pids=$(process_ids "$(ipc state 2>/dev/null)")
  ipc kill_all >/dev/null 2>&1
  for i in $(seq 1 20); do [[ "$(thread_count "$(ipc state 2>/dev/null)")" == "0" ]] && break; sleep 0.05; done
  kill_test_instances
  for p in ${=pids} $TEST_PIDS; do kill -9 $p 2>/dev/null; done
  if [[ -n $S && $S != / && -d $S && $S == /*/*/* ]]; then for p in $(pgrep -f "^$S/"); do kill -9 $p 2>/dev/null; done; fi
  if [[ $FAILS == 0 ]]; then echo "$1: all checks passed"; else echo "$1: $FAILS check(s) FAILED"; fi
  return $FAILS
}
