#!/bin/zsh
# Shared setup for the macOS demon checks (src/mac/tests/demon/*.sh). Each check is a zsh script which
# sources this file, builds a small C debuggee, writes a project file, drives a private debugger
# instance through the IPC harness (src/mac/tests/ipc_harness.sh, on top of src/mac/tests/lib.sh) and
# inspects the result through `raddbg --ipc state`, the debuggee's stdout file and the control thread's log.
# Settings: IPC_PORT (default 7950), HARNESS_DIR (default a temp dir), RADDBG (default build/raddbg).
# Never run raddbg without an --ipc_port: the user's own debugger listens on the default port.
export IPC_PORT=${IPC_PORT:-7950}
DEMON_TESTS_DIR="$(cd "$(dirname ${(%):-%x})" && pwd)"
source "$DEMON_TESTS_DIR/../ipc_harness.sh"
CTRL_LOG=$S/logs/ctrl_thread.raddbg_log
FAILS=0
# rjf: pids of processes this script started itself (register with `track_pid`); finish kills them
typeset -a TEST_PIDS
track_pid() { TEST_PIDS+=($1); }

# rjf: the debuggees are built from src/<name>.c into $S/<name> (lib.sh's build_c: two steps and a dSYM),
# and the project files carry a stdout_path ($S/<name>.stdout.txt) the checks read
BUILD_C_SRC_DIR=$DEMON_TESTS_DIR/src
PROJECT_STDOUT=1
# rjf: line number of the first line of src/<name>.c containing <marker>
line_of() { line_of_pattern "$DEMON_TESTS_DIR/src/$1.c" "$2"; }
# rjf: demon events of a kind in the control thread log (count); log_events_before <kind> <marker>: those before the first line matching <marker>
log_events() { grep -E "kind: +$1\$" "$CTRL_LOG" 2>/dev/null | wc -l | tr -d ' '; }
log_events_before() { sed -n "1,/$2/p" "$CTRL_LOG" 2>/dev/null | grep -E "kind: +$1\$" | wc -l | tr -d ' '; }
# rjf: the control thread's log is flushed a little after the state it describes is visible over IPC:
# wait_log <grep -E pattern> [tries x 100ms] polls for a line before a check reads the log
wait_log() { for i in $(seq 1 ${2:-30}); do grep -qE "$1" "$CTRL_LOG" 2>/dev/null && return 0; sleep 0.1; done; return 1; }
# rjf: record a check. `check <description> <zsh expression>`: the expression is evaluated in this shell,
# so it should be single-quoted and refer to variables by name (`[[ $(stop_symbol "$st") == main ]]`),
# never embed a dump in itself (a quote in a path or thread name would break it). a failed evaluation
# (a syntax error, say) counts as a failure and prints zsh's message. (This shadows lib.sh's
# `check <name> <expected> <observed>` of the flow suite on purpose: the demon checks assert expressions
# and print "  PASS  <description>" lines, counted in FAILS.)
check() { if eval "$2" 2>$S/check_err.txt; then echo "  PASS  $1"; else echo "  FAIL  $1"; [[ -s $S/check_err.txt ]] && sed 's/^/        /' $S/check_err.txt; FAILS=$((FAILS+1)); fi; }
# rjf: `check_eq <description> <expected> <observed>`
check_eq() { if [[ "$2" == "$3" ]]; then echo "  PASS  $1"; else echo "  FAIL  $1: expected <$2>, observed <$3>"; FAILS=$((FAILS+1)); fi; }
# rjf: end a check script: stop the debuggees the instance still holds (kill_all), the instance (dbg_stop:
# exit, then kill by pid), and the processes the script started (track_pid). debuggees detached meanwhile
# keep running by design; they are built into $S, so as a last resort the processes started from $S are
# killed, when $S is a real directory of our own (never an empty or root path).
finish() {
  local pids=$(process_ids "$(ipc state 2>/dev/null)")
  ipc kill_all >/dev/null 2>&1
  for i in $(seq 1 20); do [[ "$(thread_count "$(ipc state 2>/dev/null)")" == "0" ]] && break; sleep 0.05; done
  dbg_stop
  for p in ${=pids} $TEST_PIDS; do kill -9 $p 2>/dev/null; done
  if [[ -n $S && $S != / && -d $S && $S == /*/*/* ]]; then for p in $(pgrep -f "^$S/"); do kill -9 $p 2>/dev/null; done; fi
  if [[ $FAILS == 0 ]]; then echo "$1: all checks passed"; else echo "$1: $FAILS check(s) FAILED"; fi
  return $FAILS
}
