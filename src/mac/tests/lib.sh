#!/bin/zsh
# Shared library of the macOS test scripts (src/mac/tests): the IPC harness (ipc_harness.sh), the
# demon checks (demon/common.sh), the feature-flow suite (flows/run_flows.sh) and the proofs source
# it. It holds what they used to define each for themselves: the private instance's start and stop,
# the readers of `raddbg --ipc state`, the waits, the check reporting, the debuggee and project file
# helpers, and the Zig generator's location and build. No `--ipc` command is sent at source time.
#
# Settings (environment, all optional):
#   RADDBG       the debugger binary (default <repo>/build/raddbg)
#   IPC_PORT     the private instance's port (default 7500 + pid % 100: two concurrent runs do not
#                drive each other's instances; never the user's default port 7423)
#   HARNESS_DIR  where the user file, the logs, the project files and the built debuggees go
#                (default a new temp dir; HARNESS_DIR_OWNED=1 then, see remove_owned_harness_dir_at_exit)
#   ZIG, GENERATOR_DIR   the Zig compiler and the generator project (generator_locate)
# Variables set here: RD (the binary), P (the --ipc_port option), S (= HARNESS_DIR), RD_PID (the
# instance started by dbg_start), REPO_ROOT.
#
# Conventions: the instance is found by its listener (lsof, LISTEN on the port) and killed by that
# pid, never by a name pattern; readiness is the listener (no probe connection, no fixed sleep);
# nothing waits a fixed time for a stop: the waits poll `state`'s stop_count / running. Never run
# raddbg without an --ipc_port: the user's own debugger listens on the default port.
#
# The demon checks' `check <description> <zsh expression>` (an expression evaluated in the script's
# shell, "  PASS  <description>" lines, counted in FAILS, see demon/common.sh) is a different
# assertion from the `check <name> <expected> <observed>` here and shadows it on purpose.

REPO_ROOT="$(cd "$(dirname ${(%):-%x})/../../.." && pwd)"
RD=${RADDBG:-"$REPO_ROOT/build/raddbg"}
export IPC_PORT=${IPC_PORT:-$(( 7500 + $$ % 100 ))}
P=--ipc_port:$IPC_PORT
if [[ -n ${HARNESS_DIR:-} ]]; then HARNESS_DIR_OWNED=${HARNESS_DIR_OWNED:-0}; else HARNESS_DIR=$(mktemp -d /tmp/raddbg_tests.XXXXXX); HARNESS_DIR_OWNED=1; fi
export HARNESS_DIR HARNESS_DIR_OWNED
S=$HARNESS_DIR
mkdir -p "$S"
RD_PID=${RD_PID:-}
# a script which owns its scratch directory (lib.sh made it) removes it at exit; a caller's HARNESS_DIR is theirs.
# zsh runs the EXIT trap in subshells too (the `( nohup ... & )` which starts an instance would remove the
# directory under the instance), so the trap acts only in the main shell (`$ZSH_SUBSHELL` is 0 there).
# An alias, not a function: in zsh an EXIT trap set inside a function runs when that function returns,
# which removed the directory right away; the alias expands at the caller's top level.
alias remove_owned_harness_dir_at_exit='[[ ${HARNESS_DIR_OWNED:-0} == 1 ]] && trap "[[ \$ZSH_SUBSHELL -eq 0 ]] && rm -rf \"\$S\"" EXIT; :'

# -- the instance ----------------------------------------------------------------------------------
# ipc <command...>: any `raddbg --ipc` command against the instance on $P; the reply (and errors) on stdout
ipc() { $RD $P --ipc "$@" 2>&1; }
# field <state dump> <name>: the value of a top-level ` name: value` line
field() { echo "$1" | grep -m1 " $2:" | awk '{print $2}'; }
# listener_pid [port]: the pid of the raddbg listening on the port (default: this harness' port)
listener_pid() { lsof -nP -iTCP:${1:-$IPC_PORT} -sTCP:LISTEN 2>/dev/null | awk 'NR>1 && $1 ~ /raddbg/ {print $2; exit}'; }
# wait_listener: readiness is the instance's own listener (lsof, LISTEN on the port): no probe connection,
# no fixed sleep; records the pid in RD_PID (20 s)
wait_listener() { RD_PID=""; for i in $(seq 1 400); do RD_PID=$(listener_pid); [[ -n "$RD_PID" ]] && return 0; sleep 0.05; done; echo "no instance listens on port $IPC_PORT"; return 1; }
# wait_up: the instance answers `state` (30 s); only once something must be sent before the test's own commands
wait_up() { for i in $(seq 1 300); do ipc state | grep -q "version" && return 0; sleep 0.1; done; echo "debugger did not come up"; return 1; }
# kill_test_instances: the instance started by dbg_start is killed by its recorded pid (RD_PID); any other
# instance still listening on this harness' port (a crashed earlier run) is found by its listener and
# killed by pid too
kill_test_instances() { local pids="${RD_PID:-} $(lsof -nP -iTCP:$IPC_PORT -sTCP:LISTEN 2>/dev/null | awk 'NR>1 && $1 ~ /raddbg/ {print $2}')"; for pid in ${=pids}; do kill -9 $pid 2>/dev/null; done; for i in $(seq 1 100); do lsof -nP -iTCP:$IPC_PORT -sTCP:LISTEN 2>/dev/null | grep -q raddbg || break; sleep 0.05; done; RD_PID=""; }
# dbg_start <project> [port]: a fresh private instance with the project loaded (nothing launched): its own
# port, user file ($S/test.raddbg_user, removed first) and logs ($S/logs); stdout in $S/raddbg_stdout.txt.
# Returns once the listener is up, with the pid in RD_PID. An earlier instance on the port is killed first.
dbg_start() {
  if [[ -n ${2:-} ]]; then export IPC_PORT=$2; P=--ipc_port:$2; fi
  kill_test_instances; mkdir -p "$S"; rm -f $S/test.raddbg_user
  (nohup $RD $P --project:$1 --user:$S/test.raddbg_user --logs:$S/logs > $S/raddbg_stdout.txt 2>&1 &)
  wait_listener
}
# dbg_stop: end the instance: `exit --force_confirm:1` (quits with a live debuggee too, tears the debuggee
# down like the GUI's confirmed close, flushes the logs and saves the project) when something listens on
# the port, wait up to 5 s for that pid to go, then kill by pid whatever is left
dbg_stop() {
  local pid=$(listener_pid)
  if [[ -n "$pid" ]]; then
    ipc exit --force_confirm:1 >/dev/null 2>&1
    for i in $(seq 1 100); do kill -0 $pid 2>/dev/null || break; sleep 0.05; done
  fi
  kill_test_instances
}

# -- state readers (raddbg --ipc state) ----------------------------------------------------------
# The dump lists the stop (ip, lines), then the threads, modules, processes, call stack and
# breakpoints as sections; the readers are section-bounded (a whole-dump grep for a name or a line
# number also matches thread names, breakpoint locations and call stack entries). List-valued readers
# print one entry per line, or joined with STATE_LIST_JOIN (with a trailing separator, as `tr '\n' ' '`
# does; the flow suite sets it to a space) when that is set.
_list_join() { if [[ -n ${STATE_LIST_JOIN:-} ]]; then tr '\n' "$STATE_LIST_JOIN"; else cat; fi; }
stop_symbol()      { echo "$1" | grep -m1 "ip_voff_symbol:" | cut -d'"' -f2; }
stop_explanation() { echo "$1" | grep -m1 "explanation:" | cut -d'"' -f2; }
stop_module()      { echo "$1" | grep -m1 "ip_module:" | cut -d'"' -f2; }
stop_bp_id()       { echo "$1" | grep -m1 "bp_id:" | cut -d'"' -f2; }
exception_code()   { field "$1" exception_code; }
# the stop's innermost source line, "file.c:N" (empty when the stop has no line), and the line at a given
# inline depth (0 = the concrete function's line, i.e. the outermost call site)
stop_file_line()    { echo "$1" | sed -n '/^ lines:/,/^ threads:/p' | grep 'file_name\|line_num' | paste - - | tail -1 | awk -F'"' '{split($3,a,":"); gsub(/ /,"",a[2]); printf "%s:%s", $2, a[2]}' | sed 's#^.*/##'; }
stop_file_line_at() { echo "$1" | sed -n '/^ lines:/,/^ threads:/p' | grep 'file_name\|line_num' | paste - - | sed -n "$(( $2 + 1 ))p" | awk -F'"' '{split($3,a,":"); gsub(/ /,"",a[2]); printf "%s:%s", $2, a[2]}'; }
# the line of the selected inline frame (what the UI shows), and the line number alone
stop_selected_line() { stop_file_line_at "$1" "$(field "$1" inline_depth)"; }
stop_line()          { stop_file_line "$1" | cut -d: -f2; }
thread_count()  { echo "$1" | sed -n '/^ threads:/,/^ modules:/p' | grep -c "   id:"; }
thread_ids()    { echo "$1" | sed -n '/^ threads:/,/^ modules:/p' | grep "   id:" | awk '{print $2}' | _list_join; }
thread_names()  { echo "$1" | sed -n '/^ threads:/,/^ modules:/p' | grep "name:" | cut -d'"' -f2 | _list_join; }
frozen_flags()  { echo "$1" | sed -n '/^ threads:/,/^ modules:/p' | grep "frozen:" | awk '{print $2}' | _list_join; }
module_names()  { echo "$1" | sed -n '/^ modules:/,/^ selected_thread:/p' | grep "name:" | cut -d'"' -f2 | _list_join; }
process_count() { echo "$1" | sed -n '/^ processes:/,/^ call_stack:/p' | grep -c "   id:"; }
process_ids()   { echo "$1" | sed -n '/^ processes:/,/^ call_stack:/p' | grep "   id:" | awk '{print $2}' | _list_join; }
# the call stack as "sym@file:line[depth]" lines (inline frames carry their depth), symbols only,
# the concrete frames' symbols (one per unwind count), the frames' modules and their pc symbols
call_stack() {
  echo "$1" | sed -n '/^ call_stack:/,/^ breakpoints:/p' | grep 'inline_depth\|   symbol\|file_name\|line_num' | paste - - - - |
    awk -F'"' '{split($1,d," "); split($5,l,":"); gsub(/ /,"",l[2]); printf "%s@%s:%s[%s]\n", $2, $4, l[2], d[2]}'
}
call_stack_symbols() { call_stack "$1" | cut -d@ -f1 | _list_join; }
concrete_frames()    { call_stack "$1" | grep '\[0\]$' | cut -d@ -f1; }
frame_modules()      { echo "$1" | sed -n '/^ call_stack:/,/^ breakpoints:/p' | grep "   module:" | cut -d'"' -f2; }
frame_ip_symbols()   { echo "$1" | sed -n '/^ call_stack:/,/^ breakpoints:/p' | grep "ip_symbol:" | cut -d'"' -f2 | _list_join; }
bp_hit_count() { echo "$1" | sed -n '/^ breakpoints:/,$p' | grep -A5 "id:  *\"\\$2\"" | grep -m1 hit_count | awk '{print $2}'; }
bp_enabled()   { echo "$1" | sed -n '/^ breakpoints:/,$p' | grep -A5 "id:  *\"\\$2\"" | grep -m1 enabled | awk '{print $2}'; }
bp_count()     { echo "$1" | sed -n '/^ breakpoints:/,$p' | grep -c "   id:"; }
bp_ids()       { echo "$1" | sed -n '/^ breakpoints:/,$p' | grep "   id:" | cut -d'"' -f2 | _list_join; }

# -- waits (100 ms polls of `state`; each poll renders a frame) ----------------------------------
# wait_stop <stop_count before> [inline_depth before] [polls]: a new stop (stop_count grew), or the
# selected inline frame changed without a run (a step into at an inlined call's first instruction only
# selects the next frame); echoes the state; WAIT_STOP_POLLS (default 80, 8 s) polls unless given
wait_stop() { local before=${1:-0} depth_before=${2:-x}; for i in $(seq 1 ${3:-${WAIT_STOP_POLLS:-80}}); do st=$(ipc state); local sc=$(field "$st" stop_count); if [[ "$(field "$st" running)" == "0" && -n "$sc" ]]; then if [[ "$sc" -gt "$before" || "$(field "$st" inline_depth)" != "$depth_before" ]]; then echo "$st"; return 0; fi; fi; sleep 0.1; done; echo "$st"; return 1; }
# go_cmd <command...>: run a command which makes the thread run and wait for the next stop; echoes the state
go_cmd() { local st0=$(ipc state); local before=$(field "$st0" stop_count); before=${before:-0}; local depth_before=$(field "$st0" inline_depth); ipc "$@" >/dev/null; wait_stop $before ${depth_before:-x}; }
run_stop() { go_cmd run; }
# wait_exit: no process is left (the target ran to its end); echoes the state (15 s)
wait_exit() { for i in $(seq 1 150); do st=$(ipc state); if [[ "$(field "$st" running)" == "0" && "$(process_count "$st")" == "0" ]]; then echo "$st"; return 0; fi; sleep 0.1; done; echo "$st"; return 1; }
# wait_exit_or_stop <stop_count before>: the process ended, or a new stop happened; echoes the state (15 s)
wait_exit_or_stop() { for i in $(seq 1 150); do st=$(ipc state); local sc=$(field "$st" stop_count); if [[ "$(field "$st" running)" == "0" && ( "$(process_count "$st")" == "0" || ( -n "$sc" && "$sc" -gt "$1" ) ) ]]; then echo "$st"; return 0; fi; sleep 0.1; done; echo "$st"; return 1; }
wait_running() { for i in $(seq 1 40); do [[ "$(field "$(ipc state)" running)" == "1" ]] && return 0; sleep 0.05; done; return 1; }
# wait_no_threads [polls]: the thread list is empty and nothing runs (30 polls)
wait_no_threads() { for i in $(seq 1 ${1:-30}); do local st=$(ipc state); [[ "$(thread_count "$st")" == "0" && "$(field "$st" running)" == "0" ]] && return 0; sleep 0.1; done; return 1; }
# wait_gone <pid>: the process is gone (5 s). NOTE: never `wait` for a background process the debugger
# attached to: ptrace reparents it to the debugger, so the shell's `wait` blocks forever
wait_gone() { for i in $(seq 1 100); do kill -0 $1 2>/dev/null || return 0; sleep 0.05; done; return 1; }
# wait_pid_gone <pid> [polls]: gone, a zombie counting as gone (50 polls)
wait_pid_gone() { for i in $(seq 1 ${2:-50}); do [[ "$(ps -o stat= -p $1 2>/dev/null | tr -d ' ')" == "" || "$(ps -o stat= -p $1 2>/dev/null | tr -d ' ')" == Z* ]] && return 0; sleep 0.1; done; return 1; }
# wait_process_gone <full path> [polls]: no process of that executable is left (50 polls)
wait_process_gone() { for i in $(seq 1 ${2:-50}); do pgrep -f "^$1" >/dev/null || return 0; sleep 0.1; done; return 1; }

# -- source line lookup ---------------------------------------------------------------------------
# line_of_tag <source file> <tag>: the line carrying "// @tag" (word boundary); the flow debuggees'
# scenario lines are tagged this way, so the expected numbers come from the sources
line_of_tag() { grep -n -E "// @$2([^a-zA-Z0-9_]|\$)" "$1" | head -1 | cut -d: -f1; }
# line_of_pattern <source file> <fixed string>: the first line containing the string (sources without
# tags: the demon debuggees' `// bp_x` markers, the Zig generator's main.zig)
line_of_pattern() { grep -n -F -m1 -- "$2" "$1" | cut -d: -f1; }

# -- check reporting (the flow suite's) -----------------------------------------------------------
# check <name> <expected> <observed>: "PASS <scenario>/<name>: <observed>" when equal, else "FAIL ...:
# expected <...>, observed <...>"; check_match takes an extended regex, check_ne an unexpected value;
# note prints an observation without a verdict; check_summary <start epoch seconds> prints the totals
# and the failures and returns 1 when any check failed
typeset -gi pass_count=0 fail_count=0
typeset -ga failures
scenario=${scenario:-}
check() {
  if [[ "$2" == "$3" ]]; then pass_count+=1; echo "PASS $scenario/$1: $3";
  else fail_count+=1; failures+=("$scenario/$1"); echo "FAIL $scenario/$1: expected <$2>, observed <$3>"; fi
}
check_match() {
  if [[ "$3" =~ $2 ]]; then pass_count+=1; echo "PASS $scenario/$1: $3";
  else fail_count+=1; failures+=("$scenario/$1"); echo "FAIL $scenario/$1: expected match <$2>, observed <$3>"; fi
}
check_ne() {
  if [[ "$2" != "$3" ]]; then pass_count+=1; echo "PASS $scenario/$1: $3";
  else fail_count+=1; failures+=("$scenario/$1"); echo "FAIL $scenario/$1: expected anything but <$2>, observed <$3>"; fi
}
note() { echo "NOTE $scenario: $*"; }
check_summary() {
  echo
  echo "=== $pass_count passed, $fail_count failed in $(( $(date +%s) - ${1:-$(date +%s)} )) s"
  for f in "${failures[@]}"; do echo "  FAIL $f"; done
  return $(( fail_count > 0 ))
}

# -- debuggees and project files ------------------------------------------------------------------
# build_c <name | source.c> [output]: a -g -O0 debuggee, compiled and linked in two steps so that the
# object file (which holds the DWARF; the executable only has a debug map pointing at it) survives,
# then dsymutil links a dSYM, which the debugger's converter reads directly. CFLAGS_EXTRA / LDFLAGS_EXTRA
# are added. A bare name is $BUILD_C_SRC_DIR/<name>.c; the output defaults to $S/<name>.
build_c() {
  local src=$1 name
  if [[ $src == */* || $src == *.c ]]; then name=${${src:t}%.c}; else name=$src; src=${BUILD_C_SRC_DIR:?build_c: BUILD_C_SRC_DIR is not set}/$name.c; fi
  local out=${2:-$S/$name}
  clang -g -O0 ${=CFLAGS_EXTRA:-} -c -o "$out.o" "$src" && clang -o "$out" "$out.o" ${=LDFLAGS_EXTRA:-} && dsymutil "$out" >/dev/null 2>&1
}
# write_project <name> <executable> [arguments] [extra top-level cfg lines | -]: writes the project file
# $S/<name>.raddbg. The target's working directory is PROJECT_WORKING_DIR (default $S); with
# PROJECT_STDOUT=1 the target's stdout goes to $S/<name>.stdout.txt (removed first). Extra lines come
# from the 4th argument, or from stdin when it is `-`. (The version header line is required: a file
# without it is treated as a new, empty project.)
write_project() {
  # (never `local path`: in zsh `path` is the array tied to PATH, and the function would lose its commands)
  local name=$1 exe=$2 args=${3:-} extra=${4:-} project_file=$S/$1.raddbg
  {
    printf '// raddbg 0.9.30 project file\n\ntarget:\n{\n  executable: "%s"\n  arguments: "%s"\n  working_directory: "%s"\n' "$exe" "$args" "${PROJECT_WORKING_DIR:-$S}"
    if [[ ${PROJECT_STDOUT:-0} == 1 ]]; then printf '  stdout_path: "%s"\n' "$S/$name.stdout.txt"; fi
    printf '  enabled: 1\n}\n'
    if [[ "$extra" == "-" ]]; then cat; elif [[ -n "$extra" ]]; then printf '%s\n' "$extra"; fi
  } > "$project_file"
  [[ ${PROJECT_STDOUT:-0} == 1 ]] && rm -f "$S/$name.stdout.txt"
  return 0
}

# -- the Zig generator (the first real target, see README "Status and open issues") ---------------
# generator_locate: ZIG (default ~/.zvm/bin/zig) and GENERATOR_DIR (the project, default
# ~/Desktop/Etc/October-2026/generator); sets GENERATOR_MAIN (src/main.zig). Returns 1 when either is
# missing: the callers skip their generator scenarios then.
generator_locate() {
  export ZIG=${ZIG:-$HOME/.zvm/bin/zig}
  export GENERATOR_DIR=${GENERATOR_DIR:-$HOME/Desktop/Etc/October-2026/generator}
  GENERATOR_MAIN=$GENERATOR_DIR/src/main.zig
  [[ -x $ZIG && -f $GENERATOR_DIR/build.zig && -f $GENERATOR_MAIN ]]
}
# generator_build <prefix>: a debug build into <prefix>/bin/generator (GENERATOR_BIN) with the cache under
# <prefix>/zig-cache; the project's own directory is left untouched
generator_build() {
  GENERATOR_BIN=$1/bin/generator
  (cd "$GENERATOR_DIR" && "$ZIG" build -Doptimize=Debug --prefix "$1" --cache-dir "$1/zig-cache")
}
