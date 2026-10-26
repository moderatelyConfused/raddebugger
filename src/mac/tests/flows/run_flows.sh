#!/bin/zsh
# Feature-flow suite for the macOS debugger, driven over `raddbg --ipc` (see src/mac/README.md,
# "Driving the debugger from a script", and src/mac/tests/ipc_harness.sh).
#
#   zsh src/mac/tests/flows/run_flows.sh [scenario ...]      run all scenarios, or the named ones
#   zsh src/mac/tests/flows/run_flows.sh -l                  list scenarios
#
# Settings (environment): RADDBG (default build/raddbg), FLOWS_BUILD_DIR (debuggees, default
# build/flows, built by build_debuggees.sh when missing), IPC_PORT (default 7500; never the user's
# 7423), HARNESS_DIR (user file, logs and project files of the private instance; default a temp dir),
# FLOWS_KEEP_LOGS=1 keeps a copy of each scenario's logs under $HARNESS_DIR/logs_<scenario>,
# FLOWS_ZIG_GENERATOR (the Zig generator project, default ~/Desktop/Etc/October-2026/generator).
#
# Every check prints "PASS <scenario>/<check>: <observed>" or "FAIL <scenario>/<check>: expected
# <...>, observed <...>"; NOTE lines record observations without a verdict; the summary at the end
# lists the failures. Scenarios never sleep for a fixed time: they poll the debugger's stop counter
# (wait_stop / wait_exit). Scenario names: F* = stepping/inline-frame review, D* = demon review,
# U* = unwinder review (see the coordinator's lists); the rest are the feature flows.
set +e
here=$(cd "$(dirname ${(%):-%x})" && pwd)
repo=$(cd "$here/../../../.." && pwd)
export IPC_PORT=${IPC_PORT:-7500}
export HARNESS_DIR=${HARNESS_DIR:-$(mktemp -d /tmp/raddbg_flows.XXXXXX)}
export RADDBG=${RADDBG:-$repo/build/raddbg}
B=${FLOWS_BUILD_DIR:-$repo/build/flows}
D=$here/debuggees
GEN=$B/generator/bin/generator
GSRC=${FLOWS_ZIG_GENERATOR:-$HOME/Desktop/Etc/October-2026/generator}/src/main.zig
[[ -x $B/basic_O0 ]] || FLOWS_BUILD_DIR=$B zsh "$here/build_debuggees.sh" >/dev/null
source "$here/../ipc_harness.sh"

typeset -i pass_count=0 fail_count=0
typeset -a failures
scenario=""

# -- reporting -----------------------------------------------------------------------------------
# check <name> <expected> <observed>: PASS when equal
check() {
  if [[ "$2" == "$3" ]]; then pass_count+=1; echo "PASS $scenario/$1: $3";
  else fail_count+=1; failures+=("$scenario/$1"); echo "FAIL $scenario/$1: expected <$2>, observed <$3>"; fi
}
# check_match <name> <extended regex> <observed>
check_match() {
  if [[ "$3" =~ $2 ]]; then pass_count+=1; echo "PASS $scenario/$1: $3";
  else fail_count+=1; failures+=("$scenario/$1"); echo "FAIL $scenario/$1: expected match <$2>, observed <$3>"; fi
}
# check_ne <name> <unexpected> <observed>
check_ne() {
  if [[ "$2" != "$3" ]]; then pass_count+=1; echo "PASS $scenario/$1: $3";
  else fail_count+=1; failures+=("$scenario/$1"); echo "FAIL $scenario/$1: expected anything but <$2>, observed <$3>"; fi
}
note() { echo "NOTE $scenario: $*"; }

# -- source line lookup by tag -------------------------------------------------------------------
# line_of <source file> <tag>  -> the line number carrying "// @tag" (word boundary)
line_of() { grep -n -E "// @$2([^a-zA-Z0-9_]|\$)" "$1" | head -1 | cut -d: -f1; }
tag_loc() { echo "$(basename $1):$(line_of $1 $2)"; }

# -- state parsing (raddbg --ipc state) ----------------------------------------------------------
# the innermost line of the stop ("<file>:<line>"), and the line at a given inline depth (0 = the
# concrete function's line, i.e. the outermost call site)
stop_file_line() {
  echo "$1" | sed -n '/^ lines:/,/^ threads:/p' | grep 'file_name\|line_num' | paste - - | tail -1 | awk -F'"' '{split($3,a,":"); gsub(/ /,"",a[2]); printf "%s:%s", $2, a[2]}'
}
stop_file_line_at() {
  echo "$1" | sed -n '/^ lines:/,/^ threads:/p' | grep 'file_name\|line_num' | paste - - | sed -n "$(( $2 + 1 ))p" | awk -F'"' '{split($3,a,":"); gsub(/ /,"",a[2]); printf "%s:%s", $2, a[2]}'
}
# the line of the selected inline frame (what the UI shows)
stop_selected_line() { stop_file_line_at "$1" "$(field "$1" inline_depth)"; }
stop_symbol() { echo "$1" | grep -m1 "ip_voff_symbol:" | cut -d'"' -f2; }
stop_explanation() { echo "$1" | grep -m1 "explanation:" | cut -d'"' -f2; }
stop_module() { echo "$1" | grep -m1 "ip_module:" | cut -d'"' -f2; }
stop_bp_id() { echo "$1" | grep -m1 "bp_id:" | cut -d'"' -f2; }
exception_code() { field "$1" exception_code; }
thread_count() { echo "$1" | sed -n '/^ threads:/,/^ modules:/p' | grep -c "   id:"; }
thread_ids() { echo "$1" | sed -n '/^ threads:/,/^ modules:/p' | grep "   id:" | awk '{print $2}' | tr '\n' ' '; }
frozen_flags() { echo "$1" | sed -n '/^ threads:/,/^ modules:/p' | grep "frozen:" | awk '{print $2}' | tr '\n' ' '; }
module_names() { echo "$1" | sed -n '/^ modules:/,/^ selected_thread:/p' | grep "name:" | cut -d'"' -f2 | tr '\n' ' '; }
process_count() { echo "$1" | sed -n '/^ processes:/,/^ call_stack:/p' | grep -c "   id:"; }
process_ids() { echo "$1" | sed -n '/^ processes:/,/^ call_stack:/p' | grep "   id:" | awk '{print $2}' | tr '\n' ' '; }
# the call stack as "sym@file:line[depth]" lines (inline frames carry their depth), symbols only,
# the concrete frames' symbols (one per unwind count), and the frames' modules
call_stack() {
  echo "$1" | sed -n '/^ call_stack:/,/^ breakpoints:/p' | grep 'inline_depth\|   symbol\|file_name\|line_num' | paste - - - - |
    awk -F'"' '{split($1,d," "); split($5,l,":"); gsub(/ /,"",l[2]); printf "%s@%s:%s[%s]\n", $2, $4, l[2], d[2]}'
}
call_stack_symbols() { call_stack "$1" | cut -d@ -f1 | tr '\n' ' '; }
concrete_frames() { call_stack "$1" | grep '\[0\]$' | cut -d@ -f1; }
frame_modules() { echo "$1" | sed -n '/^ call_stack:/,/^ breakpoints:/p' | grep "   module:" | cut -d'"' -f2; }
frame_ip_symbols() { echo "$1" | sed -n '/^ call_stack:/,/^ breakpoints:/p' | grep "ip_symbol:" | cut -d'"' -f2 | tr '\n' ' '; }
bp_hit_count() { echo "$1" | sed -n '/^ breakpoints:/,$p' | grep -A5 "id:  *\"\\$2\"" | grep -m1 hit_count | awk '{print $2}'; }
bp_enabled() { echo "$1" | sed -n '/^ breakpoints:/,$p' | grep -A5 "id:  *\"\\$2\"" | grep -m1 enabled | awk '{print $2}'; }
bp_count() { echo "$1" | sed -n '/^ breakpoints:/,$p' | grep -c "   id:"; }
bp_ids() { echo "$1" | sed -n '/^ breakpoints:/,$p' | grep "   id:" | cut -d'"' -f2 | tr '\n' ' '; }

# eval helpers: value / type / msgs of an expression in the selected frame. NOTE: `--ipc eval`
# reports what the frontend has in that frame (see the TODO at RD_CmdKind_Eval): right after a
# stop the symbol lookup can report "could not be found" for a short while (the debug info's
# lookup tables are produced asynchronously), and a value in memory the frontend has not read yet
# prints as 0 until the asynchronous read lands (the eval waits a frame's spare time at most; the
# frontend's own views read the stopping thread's frame at most stops, but not after a stop which
# reports a hit held back from an earlier run, see README). ev_out retries the former and accepts
# an answer only once two consecutive evaluations agree.
ev_raw() { ipc eval "$@"; }
ev_out() {
  local out prev=""
  for i in $(seq 1 15); do
    out=$(ipc eval "$@")
    if echo "$out" | grep -q "could not be found"; then prev=""; sleep 0.1; continue; fi
    [[ -n "$prev" && "$out" == "$prev" ]] && break
    prev=$out
  done
  echo "$out"
}
ev() {
  # the value is printed escaped (\" for quotes); unescape it
  ev_out "$@" | grep -m1 " value:" | sed 's/^ value: *//' | sed 's/^"\(.*\)"$/\1/' | sed 's/\\"/"/g'
}
# warm_symbol <name>: evaluate a symbol name until the asynchronous debug info search has answered
# for it; only for recording what a name evaluates to (the control thread waits for that search
# itself when it resolves expression breakpoints, so no scenario needs it as a warm-up anymore)
warm_symbol() { for i in $(seq 1 20); do ipc eval "$1" | grep -q "could not be found" || return 0; sleep 0.1; done; return 1; }
ev_type() { ev_out "$@" | grep -m1 " type:" | sed 's/^ type: *//' | sed 's/^"\(.*\)"$/\1/'; }
ev_msgs() { ev_out "$@" | grep -m1 " msgs:" | sed 's/^ msgs: *//' | sed 's/^"\(.*\)"$/\1/'; }

# -- flow helpers ----------------------------------------------------------------------------------
# project <name> <executable> [arguments] [extra top-level cfg lines] -> writes and echoes the project file path
project() {
  local path=$HARNESS_DIR/$1.raddbg
  printf '// raddbg 0.9.30 project file\n\ntarget:\n{\n  executable: "%s"\n  arguments: "%s"\n  working_directory: "%s"\n  enabled: 1\n}\n' "$2" "${3:-}" "$B" > "$path"
  [[ -n "${4:-}" ]] && printf '%s\n' "$4" >> "$path"
  echo "$path"
}
# start_debugger <project>: a fresh private instance with the project loaded, no target launched
start_debugger() {
  kill_test_instances; rm -f $S/test.raddbg_user; rm -rf $S/logs
  (nohup $RD $P --project:$1 --user:$S/test.raddbg_user --logs:$S/logs > $S/raddbg_stdout.txt 2>&1 &)
  for i in $(seq 1 100); do nc -z -G 1 127.0.0.1 ${P#--ipc_port:} >/dev/null 2>&1 && break; sleep 0.05; done
  (ipc state >/dev/null 2>&1 &); sleep 0.2; wait_up
}
# begin <scenario name> <executable> [arguments] [extra cfg]: fresh instance, launched and stopped at the entry point
begin() {
  scenario=$1; echo "--- $scenario"
  rm -rf $S/logs
  local proj=$(project "$1" "$2" "${3:-}" "${4:-}")
  launch "$proj" >/dev/null || { check launch "launched" "launch failed"; return 1; }
  return 0
}
# run a command which makes the thread run and wait for the next stop; echoes the state
go_cmd() { local st0=$(ipc state); local before=$(field "$st0" stop_count); before=${before:-0}; local depth_before=$(field "$st0" inline_depth); ipc "$@" >/dev/null; wait_stop $before ${depth_before:-x}; }
run_stop() { go_cmd run; }
# restart and wait for the new process to stop (the old process' exit also counts as a stop)
restart_stop() {
  local before=$(field "$(ipc state)" stop_count); before=${before:-0}; ipc restart >/dev/null
  for i in $(seq 1 100); do st=$(ipc state); local sc=$(field "$st" stop_count); if [[ "$(field "$st" running)" == "0" && -n "$sc" && "$sc" -gt "$before" && "$(process_count "$st")" != "0" ]]; then echo "$st"; return 0; fi; sleep 0.1; done
  echo "$st"; return 1
}
# wait_exit: wait until no process is left (the target ran to its end); echoes the state
wait_exit() { for i in $(seq 1 150); do st=$(ipc state); if [[ "$(field "$st" running)" == "0" && "$(process_count "$st")" == "0" ]]; then echo "$st"; return 0; fi; sleep 0.1; done; echo "$st"; return 1; }
# wait_exit_or_stop <stop_count before>: the process ended, or a new stop happened; echoes the state
wait_exit_or_stop() { for i in $(seq 1 150); do st=$(ipc state); local sc=$(field "$st" stop_count); if [[ "$(field "$st" running)" == "0" && ( "$(process_count "$st")" == "0" || ( -n "$sc" && "$sc" -gt "$1" ) ) ]]; then echo "$st"; return 0; fi; sleep 0.1; done; echo "$st"; return 1; }
wait_running() { for i in $(seq 1 40); do [[ "$(field "$(ipc state)" running)" == "1" ]] && return 0; sleep 0.05; done; return 1; }
# wait_gone <pid>: wait (up to 5 s) until a process is gone. NOTE: never `wait` for a background
# process the debugger attached to: ptrace reparents it to the debugger, so the shell's `wait` blocks forever.
wait_gone() { for i in $(seq 1 100); do kill -0 $1 2>/dev/null || return 0; sleep 0.05; done; return 1; }
# finish: stop the instance (and keep its logs when asked)
finish() {
  if [[ -n ${FLOWS_KEEP_LOGS:-} ]]; then rm -rf "$S/logs_$scenario"; cp -R "$S/logs" "$S/logs_$scenario" 2>/dev/null; cp "$S/raddbg_stdout.txt" "$S/logs_$scenario/stdout.txt" 2>/dev/null; fi
  kill_test_instances
}
# a source breakpoint by tag; echoes the breakpoint's cfg id ($xx)
bp_tag() { ipc "add_breakpoint --file_path:$1:$(line_of $1 $2)" | grep -o '\$[0-9a-f]*' | head -1; }
# does a stop's innermost line match a tag? check_stop <check name> <state> <source> <tag>
check_stop() { check "$1" "$(tag_loc $3 $4)" "$(stop_file_line "$2")"; }
# a step into a function stops at its opening brace (the line of the prologue, as upstream does):
# check_brace <check name> <state> <source> <tag of the first body line> expects `line - 1`
brace_loc() { echo "$(basename $1):$(( $(line_of $1 $2) - 1 ))"; }
check_brace() { check "$1" "$(brace_loc $3 $4)" "$(stop_file_line "$2")"; }
# count ctrl events of a kind in the ui thread log
event_count() { grep -c "kind: \"$1\"" $S/logs/ui_thread.raddbg_log 2>/dev/null; }

# ===============================================================================================
# scenarios: feature flows
# ===============================================================================================
typeset -a scenarios

scenarios+=(launch_kill_restart_exit)
scenario_launch_kill_restart_exit() {
  begin launch_kill_restart_exit $B/basic_O0 || return
  local st=$(ipc state)
  check entry_symbol "main" "$(stop_symbol "$st")"
  check process_count 1 "$(process_count "$st")"
  local pid=$(process_ids "$st" | awk '{print $1}')
  check_match pid '^[0-9]+$' "$pid"
  ipc "kill --process:$pid" >/dev/null
  st=$(wait_exit); check killed_process_count 0 "$(process_count "$st")"
  check killed_exit_recorded 1 "$(field "$st" exited_process_count)"
  bp_tag $D/basic.c main_outer >/dev/null
  st=$(restart_stop); check restart_stop_at_bp "$(tag_loc $D/basic.c main_outer)" "$(stop_file_line "$st")"
  check restart_process_count 1 "$(process_count "$st")"
  check_ne restart_new_pid "$pid" "$(process_ids "$st" | awk '{print $1}')"
  ipc clear_breakpoints >/dev/null
  ipc run >/dev/null
  st=$(wait_exit); check exit_process_count 0 "$(process_count "$st")"
  check exit_code 42 "$(field "$st" last_exit_code)"
  check exited_count 2 "$(field "$st" exited_process_count)"
  finish
}

scenarios+=(attach_detach)
scenario_attach_detach() {
  scenario=attach_detach; echo "--- $scenario"
  $B/basic_O0 spin > /dev/null 2>&1 &
  local target_pid=$!
  start_debugger "$(project attach_detach $B/basic_O0 spin)" >/dev/null
  local st=$(go_cmd "attach --pid:$target_pid")
  check attached_process "$target_pid" "$(process_ids "$st" | awk '{print $1}')"
  check attached_thread_count 1 "$(thread_count "$st")"
  check_match attached_stack_has_main 'main' "$(call_stack_symbols "$st")"
  check attached_global_counter_type "int" "$(ev_type g_counter)"
  check_match attached_global_counter '^[0-9]+$' "$(ev g_counter)"
  bp_tag $D/basic.c spin_body >/dev/null
  st=$(run_stop); check_stop attached_bp_hit "$st" $D/basic.c spin_body
  check_match attached_bp_stack '^main ' "$(call_stack_symbols "$st")"
  ipc clear_breakpoints >/dev/null
  ipc "detach --process:$target_pid" >/dev/null
  for i in $(seq 1 30); do st=$(ipc state); [[ "$(process_count "$st")" == "0" ]] && break; sleep 0.1; done
  check detached_process_gone_within_3s 0 "$(process_count "$st")"
  check detached_not_running 0 "$(field "$st" running)"
  kill -0 $target_pid 2>/dev/null; check detached_target_alive 0 $?
  kill -9 $target_pid 2>/dev/null; wait_gone $target_pid
  finish
}

scenarios+=(breakpoints_O0)
scenario_breakpoints_O0() {
  begin breakpoints_O0 $B/basic_O0 || return
  local loop_bp=$(bp_tag $D/basic.c loop_body)
  check_match bp_id '^\$[0-9a-f]+$' "$loop_bp"
  local st=$(run_stop); check_stop loop_hit1 "$st" $D/basic.c loop_body
  check loop_hit1_i 0 "$(ev i)"
  check loop_hit1_bp_id "$loop_bp" "$(stop_bp_id "$st")"
  local loop_ip=$(field "$st" ip)
  st=$(run_stop); check loop_hit2_i 1 "$(ev i)"
  check hit_count_2 2 "$(bp_hit_count "$st" "$loop_bp")"
  ipc "condition_cfg --cfg:$loop_bp --string:i==3" >/dev/null
  st=$(run_stop); check_stop cond_hit "$st" $D/basic.c loop_body
  check cond_hit_i 3 "$(ev i)"
  check cond_hit_count 3 "$(bp_hit_count "$st" "$loop_bp")"
  ipc "disable_cfg --cfg:$loop_bp" >/dev/null
  st=$(ipc state); check disabled 0 "$(bp_enabled "$st" "$loop_bp")"
  # an address breakpoint at the loop body's instruction (the disabled line breakpoint must not fire)
  ipc "add_breakpoint --vaddr:$loop_ip" >/dev/null
  st=$(run_stop); check addr_bp_symbol "loop_sum" "$(stop_symbol "$st")"
  check addr_bp_ip "$loop_ip" "$(field "$st" ip)"
  check addr_bp_i 4 "$(ev i)"
  check addr_bp_stack "loop_sum main" "$(concrete_frames "$st" | head -2 | tr '\n' ' ' | sed 's/ $//')"
  ipc clear_breakpoints >/dev/null
  # a function breakpoint added while the process is live, on a name nothing has evaluated yet (the
  # control thread must wait for the asynchronous debug info search to resolve it; see README)
  ipc "add_function_breakpoint libsystem_call" >/dev/null
  st=$(run_stop); check func_bp_cold_symbol "libsystem_call" "$(stop_symbol "$st")"
  check_brace func_bp_line "$st" $D/basic.c libsystem_call
  ipc clear_breakpoints >/dev/null
  local n0=$(bp_count "$(ipc state)")
  ipc "toggle_breakpoint --file_path:$D/basic.c:$(line_of $D/basic.c inner_body)" >/dev/null
  check toggle_add $(( n0 + 1 )) "$(bp_count "$(ipc state)")"
  ipc "toggle_breakpoint --file_path:$D/basic.c:$(line_of $D/basic.c inner_body)" >/dev/null
  check toggle_remove "$n0" "$(bp_count "$(ipc state)")"
  local tmp_bp=$(bp_tag $D/basic.c inner_body)
  ipc "remove_cfg --cfg:$tmp_bp" >/dev/null
  check removed "$n0" "$(bp_count "$(ipc state)")"
  ipc clear_breakpoints >/dev/null
  ipc run >/dev/null; st=$(wait_exit); check exit_code 42 "$(field "$st" last_exit_code)"
  finish
}

scenarios+=(breakpoint_inline_O2)
scenario_breakpoint_inline_O2() {
  begin breakpoint_inline_O2 $B/basic_O2 || return
  bp_tag $D/basic.c inline_body >/dev/null
  local st=$(run_stop)
  check inline_bp_innermost "$(tag_loc $D/basic.c inline_body)" "$(stop_file_line "$st")"
  check inline_bp_concrete_line "$(tag_loc $D/basic.c one_line_loop)" "$(stop_file_line_at "$st" 0)"
  check inline_bp_symbol "one_line_loop" "$(stop_symbol "$st")"
  check inline_bp_depth 2 "$(field "$st" inline_depth)"
  check_match inline_bp_stack '^square square_sum one_line_loop main ' "$(call_stack_symbols "$st")"
  check inline_frame_lines "square@basic.c:$(line_of $D/basic.c inline_body)[2] square_sum@basic.c:$(line_of $D/basic.c inline_outer_call)[1] one_line_loop@basic.c:$(line_of $D/basic.c one_line_loop)[0]" "$(call_stack "$st" | head -3 | tr '\n' ' ' | sed 's/ $//')"
  note "inlined parameter v at the first hit: '$(ev v)' (msgs: $(ev_msgs v)); at the second hit: '$(st=$(run_stop); ev v)'"
  check inline_bp_hit_count 2 "$(bp_hit_count "$(ipc state)" "$(bp_ids "$(ipc state)" | awk '{print $1}')")"
  finish
}

scenarios+=(breakpoint_dylib)
scenario_breakpoint_dylib() {
  begin breakpoint_dylib $B/dylib_host_O0 $B/libplugin_O0.dylib || return
  bp_tag $D/plugin.c plugin_body >/dev/null
  local st=$(run_stop)
  check dylib_bp_hit "$(tag_loc $D/plugin.c plugin_body)" "$(stop_file_line "$st")"
  check dylib_bp_module "libplugin_O0.dylib" "$(stop_module "$st")"
  check_match dylib_in_modules 'libplugin_O0.dylib' "$(module_names "$st")"
  check dylib_bp_v 1 "$(ev v)"
  check dylib_stack "plugin_fn main" "$(concrete_frames "$st" | head -2 | tr '\n' ' ' | sed 's/ $//')"
  finish
}

scenarios+=(stepping_O0)
scenario_stepping_O0() {
  begin stepping_O0 $B/basic_O0 || return
  bp_tag $D/basic.c main_outer >/dev/null
  local st=$(run_stop); check_stop at_main_outer "$st" $D/basic.c main_outer
  st=$(go_cmd step_into); check_brace into_outer_brace "$st" $D/basic.c outer_call
  check into_outer_sym "outer" "$(stop_symbol "$st")"
  st=$(go_cmd step_over); check_stop into_outer "$st" $D/basic.c outer_call
  st=$(go_cmd step_into); check_brace into_middle_brace "$st" $D/basic.c middle_call
  st=$(go_cmd step_over); check_stop into_middle "$st" $D/basic.c middle_call
  st=$(go_cmd step_into); check_brace into_inner_brace "$st" $D/basic.c inner_body
  st=$(go_cmd step_over); check_stop into_inner "$st" $D/basic.c inner_body
  check nested_stack "inner middle outer main" "$(concrete_frames "$st" | head -4 | tr '\n' ' ' | sed 's/ $//')"
  st=$(go_cmd step_out); check_stop out_to_middle "$st" $D/basic.c middle_call
  check out_middle_sym "middle" "$(stop_symbol "$st")"
  st=$(go_cmd step_over); check_stop over_middle_ret "$st" $D/basic.c middle_ret
  st=$(go_cmd step_over); check_stop over_ret_to_outer "$st" $D/basic.c outer_call
  st=$(go_cmd step_over); check_stop over_to_outer_after "$st" $D/basic.c outer_after
  st=$(go_cmd step_out); check_stop out_to_main "$st" $D/basic.c main_outer
  st=$(go_cmd step_over); check_stop over_to_main_fact "$st" $D/basic.c main_fact
  st=$(go_cmd step_into); check_brace into_fact_brace "$st" $D/basic.c fact_base
  st=$(go_cmd step_over); check_stop into_fact "$st" $D/basic.c fact_base
  check fact_n 5 "$(ev n)"
  st=$(go_cmd step_over); check_stop fact_rec_line "$st" $D/basic.c fact_rec
  st=$(go_cmd step_over); check_stop fact_over_recursion "$st" $D/basic.c fact_ret
  check fact_still_n5 5 "$(ev n)"
  check fact_r 120 "$(ev r)"
  st=$(go_cmd step_out); check_stop fact_out "$st" $D/basic.c main_fact
  st=$(go_cmd step_over); check_stop at_main_tail "$st" $D/basic.c main_tail
  st=$(go_cmd step_into); check_brace into_tail_caller_brace "$st" $D/basic.c tail_caller_body
  st=$(go_cmd step_over); check_stop into_tail_caller "$st" $D/basic.c tail_caller_body
  st=$(go_cmd step_over); check_stop tail_call_line "$st" $D/basic.c tail_call
  st=$(go_cmd step_into); check_brace into_tail_target_brace "$st" $D/basic.c tail_target_body
  st=$(go_cmd step_over); check_stop into_tail_target "$st" $D/basic.c tail_target_body
  st=$(go_cmd step_out); check_stop out_of_tail_target "$st" $D/basic.c tail_call
  st=$(go_cmd step_out); check_stop out_of_tail_caller "$st" $D/basic.c main_tail
  ipc "run_to_line --file_path:$D/basic.c:$(line_of $D/basic.c one_line_loop)" >/dev/null
  st=$(wait_stop $(field "$st" stop_count) 0); check_stop run_to_line "$st" $D/basic.c one_line_loop
  check run_to_line_sym "one_line_loop" "$(stop_symbol "$st")"
  st=$(go_cmd step_over); check_stop over_one_line_loop "$st" $D/basic.c one_line_after
  check one_line_acc 44 "$(ev acc)"
  st=$(go_cmd step_out); check_stop out_to_main_one_line "$st" $D/basic.c main_one_line
  st=$(go_cmd step_over); check_stop to_main_libsystem "$st" $D/basic.c main_libsystem
  st=$(go_cmd step_into); check_brace into_libsystem_call_brace "$st" $D/basic.c libsystem_call
  st=$(go_cmd step_over); check_stop into_libsystem_call "$st" $D/basic.c libsystem_call
  st=$(go_cmd step_into)
  if [[ "$(stop_file_line "$st")" != "$(tag_loc $D/basic.c libsystem_after)" ]]; then
    note "step into on the strlen line landed in module '$(stop_module "$st")' symbol '$(stop_symbol "$st")' (stack: $(call_stack "$st" | tr '\n' ' ')); stepping out"
    check into_strlen_frame1_is_caller "libsystem_call" "$(concrete_frames "$st" | sed -n 2p)"
    st=$(go_cmd step_out)
  fi
  check_stop into_strlen_skipped "$st" $D/basic.c libsystem_after
  check libsystem_len 11 "$(ev len)"
  if [[ "$(stop_symbol "$st")" == "libsystem_call" ]]; then
    st=$(go_cmd step_out); check_stop out_libsystem "$st" $D/basic.c main_libsystem
    st=$(go_cmd step_over)
  else
    note "resynchronizing at main_locals with a breakpoint"
    ipc clear_breakpoints >/dev/null; bp_tag $D/basic.c main_locals >/dev/null; st=$(run_stop)
  fi
  check_stop at_main_locals "$st" $D/basic.c main_locals
  # instruction steps: into the call (the line's first instruction is the `bl`: clang attributes the
  # argument setup to the previous line's row), then over an instruction inside the callee
  local sym=main; for i in $(seq 1 12); do st=$(go_cmd step_into_inst); sym=$(stop_symbol "$st"); [[ $sym == locals_frame ]] && break; done
  check inst_into_call "locals_frame" "$sym"
  check_brace inst_into_call_at_entry_line "$st" $D/basic.c locals_first
  check inst_into_call_stack "locals_frame main" "$(concrete_frames "$st" | head -2 | tr '\n' ' ' | sed 's/ $//')"
  local pc0=$(ev pc)
  st=$(go_cmd step_over_inst); check inst_over_advances_4 "$(( pc0 + 4 ))" "$(ev pc)"
  check inst_over_stays_in_callee "locals_frame" "$(stop_symbol "$st")"
  st=$(go_cmd step_out); check_stop inst_out "$st" $D/basic.c main_locals
  finish
}

scenarios+=(stepping_O2)
scenario_stepping_O2() {
  begin stepping_O2 $B/basic_O2 || return
  bp_tag $D/basic.c main_tail >/dev/null
  local st=$(run_stop); check_stop at_main_tail "$st" $D/basic.c main_tail
  st=$(go_cmd step_into); check_match into_tail_caller "basic.c:($(line_of $D/basic.c tail_caller_body)|$(( $(line_of $D/basic.c tail_caller_body) - 1 )))" "$(stop_file_line "$st")"
  check into_tail_caller_sym "tail_caller" "$(stop_symbol "$st")"
  [[ "$(stop_file_line "$st")" == "$(brace_loc $D/basic.c tail_caller_body)" ]] && st=$(go_cmd step_over)
  check_stop into_tail_caller_body "$st" $D/basic.c tail_caller_body
  st=$(go_cmd step_over); check_stop tail_call_line "$st" $D/basic.c tail_call
  st=$(go_cmd step_over); check_stop over_tail_call_returns_to_main "$st" $D/basic.c main_tail
  check over_tail_sym "main" "$(stop_symbol "$st")"
  note "after the tail-call step over: $(stop_file_line "$st") sym=$(stop_symbol "$st")"
  ipc clear_breakpoints >/dev/null; bp_tag $D/basic.c one_line_loop >/dev/null
  st=$(run_stop); check_stop run_to_one_line "$st" $D/basic.c one_line_loop
  check one_line_depth 0 "$(field "$st" inline_depth)"
  st=$(go_cmd step_into)
  check_match into_inlined_line "basic.c:($(line_of $D/basic.c inline_outer_call)|$(( $(line_of $D/basic.c inline_body) - 1 ))|$(line_of $D/basic.c inline_body))" "$(stop_selected_line "$st")"
  check_match into_inlined_depth '^[12]$' "$(field "$st" inline_depth)"
  check into_inlined_sym "one_line_loop" "$(stop_symbol "$st")"
  check into_inlined_call_site "$(tag_loc $D/basic.c one_line_loop)" "$(stop_file_line_at "$st" 0)"
  note "step into the inlined call: selected depth $(field "$st" inline_depth), lines $(call_stack "$st" | head -3 | tr '\n' ' ')"
  st=$(go_cmd step_into)
  note "second step into: selected depth $(field "$st" inline_depth), line $(stop_selected_line "$st"), stack $(call_stack "$st" | head -3 | tr '\n' ' ')"
  check second_into_stays_in_function "one_line_loop" "$(stop_symbol "$st")"
  st=$(go_cmd step_over)
  check over_in_inlined_stays_in_function "one_line_loop" "$(stop_symbol "$st")"
  note "step over in the inlined code: depth $(field "$st" inline_depth), line $(stop_selected_line "$st")"
  st=$(go_cmd step_out); check_match out_of_inlined "basic.c:($(line_of $D/basic.c one_line_loop)|$(line_of $D/basic.c one_line_after)|$(line_of $D/basic.c main_one_line))" "$(stop_selected_line "$st")"
  finish
}

scenarios+=(stepping_cpp)
scenario_stepping_cpp() {
  begin stepping_cpp $B/cpp_O0 || return
  bp_tag $D/cpp.cpp cpp_virtual_call >/dev/null
  local st=$(run_stop); check_stop at_virtual_call "$st" $D/cpp.cpp cpp_virtual_call
  st=$(go_cmd step_into); check_brace into_virtual_brace "$st" $D/cpp.cpp rect_area
  check_match virtual_sym 'area' "$(stop_symbol "$st")"
  st=$(go_cmd step_over); check_stop into_virtual "$st" $D/cpp.cpp rect_area
  check this_w 3 "$(ev 'this->w')"
  st=$(go_cmd step_out); check_stop out_virtual "$st" $D/cpp.cpp cpp_virtual_call
  st=$(go_cmd step_over); check_stop at_template_call "$st" $D/cpp.cpp cpp_template_call
  check a 12 "$(ev a)"
  st=$(go_cmd step_into); check_brace into_template_brace "$st" $D/cpp.cpp twice_body
  check_match template_sym 'twice' "$(stop_symbol "$st")"
  st=$(go_cmd step_over); check_stop into_template "$st" $D/cpp.cpp twice_body
  st=$(go_cmd step_out); check_stop out_template "$st" $D/cpp.cpp cpp_template_call
  st=$(go_cmd step_over); check_stop at_lambda_def "$st" $D/cpp.cpp cpp_lambda_def
  st=$(go_cmd step_over); check_stop at_lambda_call "$st" $D/cpp.cpp cpp_lambda_call
  st=$(go_cmd step_into); check_stop into_lambda "$st" $D/cpp.cpp cpp_lambda_def
  check_match lambda_sym 'operator\(\)|lambda' "$(stop_symbol "$st")"
  st=$(go_cmd step_out); check_stop out_lambda "$st" $D/cpp.cpp cpp_lambda_call
  ipc run >/dev/null; st=$(wait_exit); check exit_code 50 "$(field "$st" last_exit_code)"
  finish
}

scenarios+=(signal_handler)
scenario_signal_handler() {
  # SIGUSR1 breaks by default (as linux_sigusr1 does) and a continue after that stop would drop the
  # signal; this flow is about the handler, so its project turns the SIGUSR1 stop off
  begin signal_handler $B/signals_O0 handler $'mac_sigusr1: 0' || return
  bp_tag $D/signals.c handler_body >/dev/null
  local st=$(run_stop); check_stop handler_bp "$st" $D/signals.c handler_body
  check handler_sym "on_sigusr1" "$(stop_symbol "$st")"
  check handler_signo 30 "$(ev signo)"
  local stack=$(call_stack_symbols "$st")
  check_match handler_stack_top '^on_sigusr1 ' "$stack"
  check_match handler_stack_through_sigtramp 'raise_usr1 main ' "$stack"
  note "stack in the handler: $(call_stack "$st" | tr '\n' ' ') modules: $(frame_modules "$st" | tr '\n' ' ')"
  st=$(go_cmd step_over); check_stop handler_step_over "$st" $D/signals.c handler_second
  check handler_hits 1 "$(ev g_handler_hits)"
  st=$(go_cmd step_out); check_match handler_step_out "signals.c:($(line_of $D/signals.c raise_call)|$(line_of $D/signals.c raise_after))" "$(stop_file_line "$st")"
  ipc run >/dev/null; st=$(wait_exit); check exit_code 0 "$(field "$st" last_exit_code)"
  finish
}

scenarios+=(longjmp)
scenario_longjmp() {
  begin longjmp $B/signals_O0 longjmp || return
  bp_tag $D/signals.c jumper_body >/dev/null
  local st=$(run_stop); check_stop jumper_bp "$st" $D/signals.c jumper_body
  st=$(go_cmd step_over); check_stop at_longjmp "$st" $D/signals.c longjmp_call
  st=$(go_cmd step_over)
  check_match over_longjmp "signals.c:($(line_of $D/signals.c setjmp_line)|$(line_of $D/signals.c after_longjmp))" "$(stop_file_line "$st")"
  check over_longjmp_sym "do_setjmp" "$(stop_symbol "$st")"
  check over_longjmp_stack "do_setjmp main" "$(concrete_frames "$st" | head -2 | tr '\n' ' ' | sed 's/ $//')"
  ipc run >/dev/null; st=$(wait_exit); check exit_code 0 "$(field "$st" last_exit_code)"
  finish
}

scenarios+=(exceptions)
scenario_exceptions() {
  # every MAC_ExceptionCodeTable entry has a `mac_*` "Break on ..." project setting (raddbg.mdesk) with
  # the table's default, so these stop at their first chance: a hardware fault as its Mach exception
  # (EXC_BAD_ACCESS 0x10001, EXC_BREAKPOINT 0x10006), a soft signal as its number (SIGABRT 6, SIGINT 2).
  # abort() and raise() stop inside libsystem_kernel's __pthread_kill (the shared cache has no RDI
  # procedures, so that frame has no symbol or line) with main's line further down the stack.
  # A continue after a reported exception stop runs with ignore_previous_exception, which the demon
  # answers with KERN_SUCCESS (a hardware exception re-executes its instruction and faults again:
  # another stop at the same pc) or PT_THUPDATE signal 0 (a soft signal is dropped), as upstream's
  # Linux demon does with PTRACE_CONT signal 0 and Win32's with DBG_CONTINUE; the exception reaches
  # the debuggee when its setting is off (D4).
  local mode code tag expl rest
  for spec in "segv:0x10001:segv_line:Access violation writing to address 0x0" \
              "abort:0x6:abort_line:SIGABRT" \
              "trap:0x10006:trap_line:EXC_BREAKPOINT" \
              "sigint:0x2:sigint_line:SIGINT"; do
    mode=${spec%%:*}; rest=${spec#*:}; code=${rest%%:*}; rest=${rest#*:}; tag=${rest%%:*}; expl=${rest#*:}
    begin exception_$mode $B/signals_O0 $mode || continue
    local st=$(run_stop)
    check exception_code $code "$(exception_code "$st")"
    check process_still_stopped 1 "$(process_count "$st")"
    check_match explanation "hit an exception.*$expl" "$(stop_explanation "$st")"
    if [[ $mode == segv || $mode == trap ]]; then
      check_stop exception_line "$st" $D/signals.c $tag
      check exception_stack_top "main" "$(concrete_frames "$st" | head -1)"
    else
      check exception_module "libsystem_kernel.dylib" "$(stop_module "$st")"
      check exception_main_frame 1 "$(call_stack "$st" | grep -c "^main@$(tag_loc $D/signals.c $tag)\[0\]$")"
    fi
    note "$mode: code $(exception_code "$st"), explanation: $(stop_explanation "$st"), stack: $(call_stack "$st" | tr '\n' ' ')"
    local ip=$(field "$st" ip) before=$(field "$st" stop_count)
    case $mode in
      segv|trap)
        # the continue re-executes the faulting instruction: the same exception at the same pc again
        ipc run >/dev/null; st=$(wait_exit_or_stop $before)
        check continue_restops_same_code $code "$(exception_code "$st")"
        check continue_restops_same_ip "$ip" "$(field "$st" ip)"
        check continue_process_alive 1 "$(process_count "$st")"
        ipc kill_all >/dev/null; wait_exit >/dev/null;;
      sigint)
        # the continue drops the signal: raise() returns and the process exits normally
        ipc run >/dev/null; st=$(wait_exit)
        check continue_drops_signal_exits 0 "$(process_count "$st")"
        check continue_drops_signal_exit_code 0 "$(field "$st" last_exit_code)";;
      abort)
        ipc run >/dev/null; st=$(wait_exit_or_stop $before)
        note "abort after continue: processes=$(process_count "$st") exit_code=$(field "$st" last_exit_code) code=$(exception_code "$st") expl=$(stop_explanation "$st")"
        ipc kill_all >/dev/null; wait_exit >/dev/null;;
    esac
    finish
  done
}

scenarios+=(halt)
scenario_halt() {
  begin halt $B/basic_O0 spin || return
  local st0=$(ipc state); local before=$(field "$st0" stop_count); local depth=$(field "$st0" inline_depth)
  ipc run >/dev/null
  wait_running; check running 1 "$(field "$(ipc state)" running)"
  ipc halt >/dev/null
  local st=$(wait_stop $before $depth)
  check halted 0 "$(field "$st" running)"
  check halt_process_alive 1 "$(process_count "$st")"
  check_match halt_stack 'main' "$(call_stack_symbols "$st")"
  check_match halt_counter '^[0-9]+$' "$(ev g_counter)"
  bp_tag $D/basic.c spin_body >/dev/null
  st=$(run_stop); check_stop after_halt_bp "$st" $D/basic.c spin_body
  ipc kill_all >/dev/null; st=$(wait_exit); check killed 0 "$(process_count "$st")"
  finish
}

scenarios+=(eval_O0)
scenario_eval_O0() {
  begin eval_O0 $B/basic_O0 || return
  local bp=$(bp_tag $D/basic.c inner_body)
  ipc "condition_cfg --cfg:$bp --string:a==10" >/dev/null
  local st=$(run_stop); check_stop at_inner_from_locals "$st" $D/basic.c inner_body
  check local_a 10 "$(ev a)"
  check stack "inner locals_frame main" "$(concrete_frames "$st" | head -3 | tr '\n' ' ' | sed 's/ $//')"
  ipc "select_unwind --unwind_count:1 --inline_depth:0" >/dev/null
  st=$(ipc state); check selected_unwind 1 "$(field "$st" unwind_count)"
  check outer_b 20 "$(ev b)"
  check outer_pt "{10, 20}" "$(ev pt | sed 's/  */ /g')"
  check outer_arr "[10, 20, 30]" "$(ev arr | sed 's/  */ /g')"
  check_match outer_str '"frame-string"' "$(ev str)"
  note "outer frame pointer reads: rec->name='$(ev 'rec->name')' (*rec).name='$(ev '(*rec).name')' rec->pt.y=$(ev 'rec->pt.y') (expected \"rec\", \"rec\", 4)"
  check outer_rec_pt_y 4 "$(ev 'rec->pt.y')"
  check outer_arr2 30 "$(ev 'arr[2]')"
  check outer_sizeof_pt 8 "$(ev 'sizeof(pt)')"
  check outer_type_pt "Point" "$(ev_type pt)"
  local outer_pc=$(ev pc) outer_sp=$(ev sp)
  ipc "select_unwind --unwind_count:0 --inline_depth:0" >/dev/null
  check_ne inner_pc_differs "$outer_pc" "$(ev pc)"
  check_match pc_decimal '^[0-9]+$' "$(ev pc)"
  check_match sp_decimal '^[0-9]+$' "$(ev sp)"
  check_match lr_decimal '^[0-9]+$' "$(ev lr)"
  check x0_is_a 10 "$(ev x0)"
  check pc_matches_ip "$(field "$st" ip)" "$(printf '0x%x' $(ev pc))"
  check global_counter 15 "$(ev g_counter)"
  check static_counter 100 "$(ev s_static_counter)"
  note "thread-local t_tls_counter before the thread's first access of the image's TLVs: '$(ev t_tls_counter)' (the block is allocated lazily; U5 checks it after the access)"
  check_match global_message '"hello flows"' "$(ev g_message)"
  check global_array "[0, 1, 2, 3, 4, 5, 6, 7]" "$(ev g_array | sed 's/  */ /g')"
  check global_array_elem 5 "$(ev 'g_array[5]')"
  check_match global_record '"rec".*\{3, 4\}.*\[1, 2, 3, 4\].*2\.5' "$(ev g_record | sed 's/  */ /g')"
  check global_record_pt_y 4 "$(ev 'g_record.pt.y')"
  check_match global_record_next '^0(x0)?$' "$(ev 'g_record.next' | awk '{print $1}')"
  check_match global_addr_type '^int(32)? \*$' "$(ev_type '&g_counter')"
  check watched 10 "$(ev g_watched)"
  check arithmetic 15 "$(ev 'a + 5')"
  check bad_expr_msgs_nonempty 1 "$([[ -n "$(ev_msgs no_such_symbol_xyz)" ]] && echo 1 || echo 0)"
  st=$(go_cmd step_out); check_stop out_to_locals_frame "$st" $D/basic.c locals_call
  check after_stepout_b 20 "$(ev b)"
  check after_stepout_r_type "int" "$(ev_type r)"
  st=$(go_cmd step_over); check_stop locals_after "$st" $D/basic.c locals_after
  check r_after 11 "$(ev r)"
  finish
}

scenarios+=(callstack_O2_nofp)
scenario_callstack_O2_nofp() {
  begin callstack_O2_nofp $B/basic_O2_nofp || return
  bp_tag $D/basic.c inner_body >/dev/null
  local st=$(run_stop)
  check nofp_bp_symbol "inner" "$(stop_symbol "$st")"
  check nofp_stack "inner middle outer main" "$(concrete_frames "$st" | head -4 | tr '\n' ' ' | sed 's/ $//')"
  st=$(go_cmd step_out); check nofp_step_out_sym "middle" "$(stop_symbol "$st")"
  check nofp_stack_after_out "middle outer main" "$(concrete_frames "$st" | head -3 | tr '\n' ' ' | sed 's/ $//')"
  finish
}

scenarios+=(threads_breakpoints)
scenario_threads_breakpoints() {
  begin threads_breakpoints $B/threads_O0 || return
  bp_tag $D/threads.c worker_body >/dev/null
  local st tids="" idxs="" i
  for i in 1 2 3 4; do
    st=$(run_stop)
    if [[ "$(process_count "$st")" == "0" ]]; then note "the process ended after $(( i - 1 )) of 4 worker breakpoint hits (the other threads' hits were lost)"; break; fi
    check_stop "worker_hit_$i" "$st" $D/threads.c worker_body
    tids="$tids$(field "$st" selected_thread) "
    idxs="$idxs$(ev idx) "
  done
  check distinct_threads 4 "$(echo $tids | tr ' ' '\n' | sort -u | grep -c .)"
  check worker_indices "0 1 2 3" "$(echo $idxs | tr ' ' '\n' | grep . | sort -n | tr '\n' ' ' | sed 's/ $//')"
  check_match worker_stack '^worker_body worker ' "$(call_stack_symbols "$st")"
  check thread_count_at_stop 5 "$(thread_count "$st")"
  ipc run >/dev/null; st=$(wait_exit); check exit_code 0 "$(field "$st" last_exit_code)"
  check new_thread_events 5 "$(event_count NewThread)"
  check end_thread_events 5 "$(event_count EndThread)"
  finish
}

scenarios+=(threads_freeze)
scenario_threads_freeze() {
  begin threads_freeze $B/threads_O0 spin || return
  bp_tag $D/threads.c main_spin >/dev/null
  local st=$(run_stop); check_stop main_spin_hit "$st" $D/threads.c main_spin
  check two_threads 2 "$(thread_count "$st")"
  local main_tid=$(field "$st" selected_thread)
  local spin_tid=$(thread_ids "$st" | tr ' ' '\n' | grep -v "^$main_tid$" | grep . | head -1)
  check_match spinner_tid '^[0-9]+$' "$spin_tid"
  ipc "select_thread --thread:$spin_tid" >/dev/null
  st=$(ipc state); check selected_spinner "$spin_tid" "$(field "$st" selected_thread)"
  check_match spinner_stack 'spinner' "$(call_stack_symbols "$st")"
  ipc "select_thread --thread:$main_tid" >/dev/null
  ipc "freeze_thread --thread:$spin_tid" >/dev/null
  st=$(ipc state); check_match frozen_flag '1' "$(frozen_flags "$st")"
  local c1=$(ev g_spin_counter)
  st=$(run_stop); st=$(run_stop)
  local c2=$(ev g_spin_counter)
  check frozen_counter_unchanged "$c1" "$c2"
  check_stop frozen_main_runs "$st" $D/threads.c main_spin
  ipc "thaw_thread --thread:$spin_tid" >/dev/null
  st=$(ipc state); check thawed_flags "0 0 " "$(frozen_flags "$st")"
  st=$(run_stop); st=$(run_stop)
  check_ne thawed_counter_advances "$c2" "$(ev g_spin_counter)"
  ipc kill_all >/dev/null; wait_exit >/dev/null
  finish
}

scenarios+=(modules)
scenario_modules() {
  begin modules $B/dylib_host_O0 $B/libplugin_O0.dylib || return
  bp_tag $D/dylib_host.c call_plugin >/dev/null
  bp_tag $D/dylib_host.c after_dlclose >/dev/null
  local st=$(run_stop); check_stop at_call_plugin "$st" $D/dylib_host.c call_plugin
  check_match loaded_after_dlopen 'libplugin_O0.dylib' "$(module_names "$st")"
  check plugin_counter_eval 0 "$(ev plugin_counter)"
  st=$(run_stop); check_stop at_after_dlclose "$st" $D/dylib_host.c after_dlclose
  check unloaded_after_dlclose 0 "$(module_names "$st" | grep -c libplugin)"
  st=$(run_stop); check_stop second_round "$st" $D/dylib_host.c call_plugin
  check_match reloaded 'libplugin_O0.dylib' "$(module_names "$st")"
  # rjf: the after_dlclose breakpoint is still set: it hits in the second round too, before the exit
  st=$(run_stop); check_stop at_after_dlclose_2 "$st" $D/dylib_host.c after_dlclose
  ipc run >/dev/null; st=$(wait_exit); check exit_code 8 "$(field "$st" last_exit_code)"
  note "module events: NewModule=$(event_count NewModule) EndModule=$(event_count EndModule); dyld notification: $(grep -c 'could not find _dyld_debugger_notification' $S/logs/ctrl_thread.raddbg_log 2>/dev/null) 'not found' log lines"
  finish
}

scenarios+=(fork)
scenario_fork() {
  begin fork $B/fork_exec_O0 fork || return
  bp_tag $D/fork_exec.c child_work_body >/dev/null
  bp_tag $D/fork_exec.c parent_after >/dev/null
  local pid=$(process_ids "$(ipc state)" | awk '{print $1}')
  local st=$(run_stop)
  note "D2 first stop after fork: $(stop_file_line "$st") processes=$(process_count "$st") expl=$(stop_explanation "$st")"
  check_stop D2_parent_after_wait "$st" $D/fork_exec.c parent_after
  check D2_parent_marker 0 "$(ev g_fork_marker)"
  check D2_one_process 1 "$(process_count "$st")"
  check D2_no_child_left 0 "$(pgrep -P $pid 2>/dev/null | wc -l | tr -d ' ')"
  check D2_no_spinning_child "" "$(ps -axo pid,%cpu,comm | grep "fork_exec" | awk '$2 > 50.0 {print $1}' | tr '\n' ' ')"
  ipc run >/dev/null; st=$(wait_exit)
  check D2_exit_code_from_child 7 "$(field "$st" last_exit_code)"
  finish
}

scenarios+=(exec)
scenario_exec() {
  begin exec $B/fork_exec_O0 "exec $B/basic_O0" || return
  bp_tag $D/fork_exec.c before_exec >/dev/null
  local st=$(run_stop); check_stop before_exec "$st" $D/fork_exec.c before_exec
  bp_tag $D/basic.c main_outer >/dev/null
  st=$(go_cmd run)
  note "exec first stop: $(stop_file_line "$st") module=$(stop_module "$st") expl=$(stop_explanation "$st") code=$(exception_code "$st") modules=$(module_names "$st" | tr ' ' '\n' | grep -c .)"
  if [[ "$(stop_file_line "$st")" != "$(tag_loc $D/basic.c main_outer)" && "$(process_count "$st")" == "1" ]]; then
    st=$(go_cmd run)
  fi
  check_stop after_exec_bp "$st" $D/basic.c main_outer
  check_match exec_module 'basic_O0' "$(stop_module "$st")"
  check_match exec_modules_replaced 'basic_O0' "$(module_names "$st")"
  check exec_old_image_gone 0 "$(module_names "$st" | grep -c fork_exec)"
  ipc run >/dev/null; st=$(wait_exit); check exit_code 42 "$(field "$st" last_exit_code)"
  note "exec: an exit code of 100+n means execv failed with errno n (EPERM 1, EACCES 13, ENOENT 2)"
  finish
}

scenarios+=(watchpoint)
scenario_watchpoint() {
  begin watchpoint $B/basic_O0 || return
  bp_tag $D/basic.c main_loop >/dev/null
  local st=$(run_stop); check_stop at_main_loop "$st" $D/basic.c main_loop
  ipc "break_when_value_changes g_watched" >/dev/null
  st=$(ipc state); check watch_bp_count 2 "$(bp_count "$st")"
  st=$(run_stop)
  check_match watch_hit1_line "basic.c:($(line_of $D/basic.c watch_write)|$(line_of $D/basic.c loop_body))" "$(stop_file_line "$st")"
  check watch_hit1_sym "loop_sum" "$(stop_symbol "$st")"
  check watch_hit1_value 0 "$(ev g_watched)"
  note "watch stop 1: ip=$(field "$st" ip) i=$(ev i) sum=$(ev sum) g_watched=$(ev g_watched)"
  st=$(run_stop); check watch_hit2_value 1 "$(ev g_watched)"
  note "watch stop 2: ip=$(field "$st" ip) i=$(ev i) sum=$(ev sum) g_watched=$(ev g_watched)"
  st=$(run_stop); check watch_hit3_value 3 "$(ev g_watched)"
  check watch_hit3_i 2 "$(ev i)"
  note "watch stop 3: ip=$(field "$st" ip) i=$(ev i) sum=$(ev sum) g_watched=$(ev g_watched)"
  ipc clear_breakpoints >/dev/null
  ipc run >/dev/null; st=$(wait_exit); check exit_code 42 "$(field "$st" last_exit_code)"
  finish
}

scenarios+=(zig_generator)
scenario_zig_generator() {
  if [[ ! -x $GEN || ! -f $GSRC ]]; then scenario=zig_generator; note "generator not built; skipped"; return; fi
  begin zig_generator $GEN || return
  ipc "add_breakpoint --file_path:$GSRC:57" >/dev/null
  local st=$(run_stop)
  check bp57 "main.zig:57" "$(stop_selected_line "$st")"
  check bp57_frame_is_main "main" "$(stop_symbol "$st" | sed 's/.*\.//')"
  st=$(go_cmd step_into)
  check_match into_next 'next' "$(stop_symbol "$st")"
  local l1=$(stop_file_line "$st")
  st=$(go_cmd step_over); local l2=$(stop_file_line "$st")
  check_ne over_contextswitch_moves "$l1" "$l2"
  check_match over_contextswitch_in_next 'next' "$(stop_symbol "$st")"
  st=$(go_cmd step_out)
  check out_to_main "main" "$(stop_symbol "$st" | sed 's/.*\.//')"
  check_match out_line 'main.zig:(57|58)' "$(stop_file_line "$st")"
  ipc clear_breakpoints >/dev/null
  ipc "add_breakpoint --file_path:$GSRC:4" >/dev/null
  st=$(run_stop)
  check_match fiber_bp 'range' "$(stop_symbol "$st")"
  check_match fiber_stack 'range' "$(call_stack_symbols "$st")"
  note "fiber call stack: $(call_stack "$st" | tr '\n' ' ') modules: $(frame_modules "$st" | tr '\n' ' ')"
  finish
}

# ===============================================================================================
# scenarios: stepping / inline-frame review (F*)
# ===============================================================================================

scenarios+=(F1_inline_seq_O2)
scenario_F1_inline_seq_O2() {
  begin F1_inline_seq_O2 $B/inline_flows_O2 || return
  local src=$D/inline_flows.c
  # F1: breakpoint on next()'s last line (inline depth 1), step over -> main's second call line at depth 0
  bp_tag $src next_last >/dev/null
  local st=$(run_stop)
  check F1_bp_line "$(tag_loc $src next_last)" "$(stop_selected_line "$st")"
  check F1_bp_depth 1 "$(field "$st" inline_depth)"
  check F1_bp_call_site "$(tag_loc $src main_next1)" "$(stop_file_line_at "$st" 0)"
  st=$(go_cmd step_over)
  check F1_step_over_line "$(tag_loc $src main_next2)" "$(stop_selected_line "$st")"
  check F1_step_over_depth 0 "$(field "$st" inline_depth)"
  check F1_step_over_frame "main@inline_flows.c:$(line_of $src main_next2)[0]" "$(call_stack "$st" | grep '\[0\]$' | head -1)"
  note "F1 after step over: lines $(echo "$st" | sed -n '/^ lines:/,/^ threads:/p' | grep 'file_name\|line_num' | paste - - | awk -F'"' '{split($3,a,":"); gsub(/ /,"",a[2]); printf "%s:%s ", $2, a[2]}') stack: $(call_stack "$st" | tr '\n' ' ')"
  # F7: with a disassembly tab focused, a step into here executes one instruction (not just a frame change)
  local pc0=$(ev pc)
  ipc go_to_disassembly >/dev/null
  st=$(go_cmd step_into)
  check F7_pc_advanced_one_instruction "$(( pc0 + 4 ))" "$(ev pc)"
  check_ne F7_not_only_frame_change "$pc0" "$(ev pc)"
  note "F7: pc before $(printf '0x%x' $pc0), after $(printf '0x%x' $(ev pc)), depth $(field "$st" inline_depth), stop_count: $(field "$st" stop_count)"
  # F1b: inside next(), stepping over a line whose successor begins with inlined helper() stays at next's line, depth 1
  ipc clear_breakpoints >/dev/null; bp_tag $src next_first >/dev/null
  st=$(restart_stop)
  check F1b_bp "$(tag_loc $src next_first)" "$(stop_selected_line "$st")"
  check F1b_bp_depth 1 "$(field "$st" inline_depth)"
  st=$(go_cmd step_over)
  check F1b_step_over_line "$(tag_loc $src next_helper_call)" "$(stop_selected_line "$st")"
  check F1b_step_over_depth 1 "$(field "$st" inline_depth)"
  note "F1b after step over: innermost $(stop_file_line "$st"), depth $(field "$st" inline_depth), stack: $(call_stack "$st" | tr '\n' ' ')"
  finish
}

scenarios+=(F2_inline_seq_repeat)
scenario_F2_inline_seq_repeat() {
  begin F2_inline_seq_repeat $B/inline_flows_O2 || return
  local src=$D/inline_flows.c; bp_tag $src next_last >/dev/null
  local depths="" lines="" i st
  for i in $(seq 1 20); do
    st=$(restart_stop); st=$(go_cmd step_over)
    depths="$depths$(field "$st" inline_depth)"; lines="$lines $(stop_selected_line "$st")"
  done
  check F2_depth_identical_20x "00000000000000000000" "$depths"
  check F2_one_distinct_line 1 "$(echo $lines | tr ' ' '\n' | sort -u | grep -c .)"
  note "F2 lines: $(echo $lines | tr ' ' '\n' | sort | uniq -c | tr '\n' ';')"
  finish
}

scenarios+=(F3_inline_fresh_rdi)
scenario_F3_inline_fresh_rdi() {
  scenario=F3_inline_fresh_rdi; echo "--- $scenario"
  local src=$D/inline_flows.c
  local rdi=$B/inline_flows_O2.dSYM/Contents/Resources/DWARF/inline_flows_O2.rdi   # the converter writes next to the DWARF file it reads
  rm -f $rdi; rm -rf $S/logs
  check F3_rdi_deleted 0 "$(find $B -name 'inline_flows_O2*.rdi' | wc -l | tr -d ' ')"
  start_debugger "$(project F3_inline_fresh_rdi $B/inline_flows_O2)" >/dev/null
  ipc "add_breakpoint --file_path:$src:$(line_of $src main_next1)" >/dev/null
  local st=$(go_cmd run)
  check F3_first_stop_is_breakpoint 1 "$(stop_explanation "$st" | grep -c breakpoint)"
  check F3_first_stop_selected_line "$(tag_loc $src main_next1)" "$(stop_selected_line "$st")"
  check F3_first_stop_depth 0 "$(field "$st" inline_depth)"
  check F3_innermost_is_next_first "$(tag_loc $src next_first)" "$(stop_file_line "$st")"
  check F3_rdi_created 1 "$(find $B -name 'inline_flows_O2*.rdi' | wc -l | tr -d ' ')"
  finish
}

scenarios+=(F4a_inline_recursion_O2)
scenario_F4a_inline_recursion_O2() {
  begin F4a_inline_recursion_O2 $B/inline_rec_O2 || return
  local src=$D/inline_rec.c
  bp_tag $src f_rec_call >/dev/null
  local st=$(run_stop)
  check F4a_bp_in_g "g" "$(stop_symbol "$st")"
  check F4a_bp_depth 1 "$(field "$st" inline_depth)"
  check F4a_bp_line "$(tag_loc $src f_rec_call)" "$(stop_selected_line "$st")"
  check F4a_n 4 "$(ev n)"
  ipc clear_breakpoints >/dev/null
  st=$(go_cmd step_over)
  check F4a_step_over_line "$(tag_loc $src f_after)" "$(stop_selected_line "$st")"
  check F4a_step_over_sym "g" "$(stop_symbol "$st")"
  check F4a_step_over_depth 1 "$(field "$st" inline_depth)"
  note "F4a: r after the recursion (register-located at -O2): '$(ev r)' (msgs: $(ev_msgs r)); expected 7"
  note "F4a after step over: $(stop_file_line "$st") depth $(field "$st" inline_depth) stack: $(call_stack "$st" | tr '\n' ' ')"
  finish
}

scenarios+=(F4b_inline_same_line_O2)
scenario_F4b_inline_same_line_O2() {
  begin F4b_inline_same_line_O2 $B/inl_main_O2 || return
  ipc "add_breakpoint --file_path:$D/inl.h:3" >/dev/null
  local st=$(run_stop)
  check F4b_bp "inl.h:3" "$(stop_selected_line "$st")"
  check F4b_bp_depth 1 "$(field "$st" inline_depth)"
  check F4b_call_site "inl_main.c:3" "$(stop_file_line_at "$st" 0)"
  st=$(go_cmd step_over)
  check F4b_step_over "inl.h:4" "$(stop_selected_line "$st")"
  note "F4b after step over: innermost $(stop_file_line "$st") depth $(field "$st" inline_depth) stack: $(call_stack "$st" | tr '\n' ' ')"
  finish
}

scenarios+=(F5a_coro_switch_O0)
scenario_F5a_coro_switch_O0() {
  begin F5a_coro_switch_O0 $B/coro_O0 || return
  local src=$D/coro.c; bp_tag $src main_switch >/dev/null
  local st=$(run_stop); check_stop at_switch "$st" $src main_switch
  ipc clear_breakpoints >/dev/null
  st=$(go_cmd step_over)
  check F5a_step_over_returns_to_parent "$(tag_loc $src main_after_switch)" "$(stop_file_line "$st")"
  check F5a_sym "main" "$(stop_symbol "$st")"
  check F5a_trace 1 "$(ev g_co_trace)"
  note "F5a after step over: $(stop_file_line "$st") sym=$(stop_symbol "$st") stack: $(call_stack "$st" | tr '\n' ' ')"
  ipc run >/dev/null; st=$(wait_exit); check exit_code 6 "$(field "$st" last_exit_code)"
  finish
}

scenarios+=(F5b_bigframe_step_O2)
scenario_F5b_bigframe_step_O2() {
  begin F5b_bigframe_step_O2 $B/bigframe_O2 || return
  local src=$D/bigframe.c
  ipc "add_function_breakpoint big_frame" >/dev/null
  local st=$(run_stop); check F5b_at_entry "big_frame" "$(stop_symbol "$st")"
  check_stop F5b_entry_line "$st" $src big_first_line
  ipc clear_breakpoints >/dev/null
  st=$(go_cmd step_over)
  check F5b_still_in_process 1 "$(process_count "$st")"
  check_stop F5b_step_over_next_line "$st" $src big_second_line
  note "F5b after step over: $(stop_file_line "$st") sym=$(stop_symbol "$st") processes=$(process_count "$st") exit_code=$(field "$st" last_exit_code)"
  finish
}

scenarios+=(F6_visit_qsort_O2)
scenario_F6_visit_qsort_O2() {
  begin F6_visit_qsort_O2 $B/visit_qsort_O2 || return
  local src=$D/visit_qsort.c; bp_tag $src visit_line >/dev/null
  local st=$(run_stop); check_stop at_visit_line "$st" $src visit_line
  note "F6: node at the breakpoint (register-located at -O2): node='$(ev node)' node->value='$(ev 'node->value')' msgs: $(ev_msgs 'node->value')"
  local sp0=$(ev sp)
  ipc clear_breakpoints >/dev/null
  st=$(go_cmd step_into)
  note "F6 step into stopped at $(stop_file_line "$st") sym=$(stop_symbol "$st") depth=$(field "$st" inline_depth) sp=$(ev sp) (sp before: $sp0) stack: $(call_stack "$st" | tr '\n' ' ')"
  check F6_same_recursion_level_sp "$sp0" "$(ev sp)"
  check F6_concrete_frame_is_root_visit "visit" "$(concrete_frames "$st" | head -1)"
  note "F6: node->value after the step into: '$(ev 'node->value')'"
  finish
}

# ===============================================================================================
# scenarios: demon review (D*)
# ===============================================================================================

scenarios+=(D1_detach_launched)
scenario_D1_detach_launched() {
  local exe=$GEN; [[ -x $GEN && -f $GSRC ]] || exe=$B/basic_O0
  begin D1_detach_launched $exe || return
  local st=$(ipc state); local pid=$(process_ids "$st" | awk '{print $1}')
  if [[ $exe == $GEN ]]; then ipc "add_breakpoint --file_path:$GSRC:57" >/dev/null; else bp_tag $D/basic.c main_outer >/dev/null; fi
  st=$(run_stop); check D1_bp_hit 1 "$(stop_explanation "$st" | grep -c breakpoint)"
  ipc clear_breakpoints >/dev/null
  ipc "detach --process:$pid" >/dev/null
  for i in $(seq 1 30); do st=$(ipc state); [[ "$(process_count "$st")" == "0" ]] && break; sleep 0.1; done
  check D1_process_gone_within_3s 0 "$(process_count "$st")"
  check D1_not_running 0 "$(field "$st" running)"
  for i in $(seq 1 50); do kill -0 $pid 2>/dev/null || break; sleep 0.1; done
  kill -0 $pid 2>/dev/null; check D1_debuggee_ran_to_completion 1 $?
  kill -9 $pid 2>/dev/null
  finish
}

scenarios+=(D1_detach_two_processes)
scenario_D1_detach_two_processes() {
  begin D1_detach_two_processes $B/basic_O0 spin || return
  $B/basic_O0 spin >/dev/null 2>&1 &
  local second=$!
  local st=$(go_cmd "attach --pid:$second")
  check two_processes 2 "$(process_count "$st")"
  local first=$(process_ids "$st" | tr ' ' '\n' | grep -v "^$second$" | grep . | head -1)
  ipc "detach --process:$second" >/dev/null
  for i in $(seq 1 30); do st=$(ipc state); [[ "$(process_count "$st")" == "1" ]] && break; sleep 0.1; done
  check detached_one 1 "$(process_count "$st")"
  check detached_not_running 0 "$(field "$st" running)"
  bp_tag $D/basic.c spin_body >/dev/null
  st=$(run_stop); check_stop remaining_process_breaks "$st" $D/basic.c spin_body
  check remaining_pid "$first" "$(process_ids "$st" | awk '{print $1}')"
  kill -9 $second 2>/dev/null; wait_gone $second
  ipc kill_all >/dev/null; wait_exit >/dev/null
  finish
}

scenarios+=(D3_modules_system)
scenario_D3_modules_system() {
  begin D3_modules_system $B/basic_O0 || return
  local st=$(ipc state)
  check_match D3_libsystem_in_modules 'libSystem.B.dylib' "$(module_names "$st")"
  check_match D3_libsystem_kernel_in_modules 'libsystem_kernel.dylib' "$(module_names "$st")"
  warm_symbol write; note "D3: eval write -> $(ipc eval write | grep -m1 'value\|msgs' | tr -d '\n')"
  ipc "add_function_breakpoint write" >/dev/null
  st=$(run_stop)
  check D3_write_bp_module "libsystem_kernel.dylib" "$(stop_module "$st")"
  check_match D3_write_bp_stack 'main' "$(call_stack_symbols "$st")"
  note "D3 NewModule events: $(event_count NewModule); dyld notification 'not found' lines: $(grep -c 'could not find _dyld_debugger_notification' $S/logs/ctrl_thread.raddbg_log 2>/dev/null)"
  finish
}

scenarios+=(D4_segv_recover)
scenario_D4_segv_recover() {
  # the fault settings off: the first chance is passed (KERN_FAILURE), the kernel's SIGSEGV reaches
  # the handler, no stop
  begin D4_segv_recover $B/signals_O0 segv_recover $'mac_sigsegv: 0\nmac_sigbus: 0\nmac_exc_bad_access: 0' || return
  local before=$(field "$(ipc state)" stop_count)
  ipc run >/dev/null
  local st=$(wait_exit_or_stop $before)
  note "D4 after run: processes=$(process_count "$st") expl=$(stop_explanation "$st") code=$(exception_code "$st")"
  check D4_no_stop_on_filtered_fault 0 "$(process_count "$st")"
  check D4_exit_code 0 "$(field "$st" last_exit_code)"
  check D4_recovered_printed 1 "$(grep -c recovered $S/raddbg_stdout.txt)"
  finish
  # the defaults (EXC_BAD_ACCESS breaks): the first chance stops at the faulting line; a continue
  # re-executes the store (ignore_previous_exception -> KERN_SUCCESS), which faults again and stops
  # again at the same pc, so the handler runs only with the settings off (above), as upstream's
  # Linux and Win32 demons behave (see the exceptions scenario)
  begin D4_segv_recover_default $B/signals_O0 segv_recover || return
  st=$(run_stop)
  check D4_default_first_chance_code 0x10001 "$(exception_code "$st")"
  check_stop D4_default_fault_line "$st" $D/signals.c recover_fault
  check D4_default_process_alive 1 "$(process_count "$st")"
  local ip=$(field "$st" ip)
  st=$(go_cmd run)
  check D4_default_continue_restops_code 0x10001 "$(exception_code "$st")"
  check D4_default_continue_same_ip "$ip" "$(field "$st" ip)"
  check D4_default_continue_process_alive 1 "$(process_count "$st")"
  note "D4 default settings after one continue: 'recovered' printed $(grep -c recovered $S/raddbg_stdout.txt) time(s) (the handler runs when mac_exc_bad_access is off)"
  ipc kill_all >/dev/null; wait_exit >/dev/null
  finish
}

scenarios+=(D5_threads_fault_frozen_reply)
scenario_D5_threads_fault_frozen_reply() {
  begin D5_threads_fault_frozen_reply $B/threads_O0 fault || return
  local st=$(run_stop)
  check_match D5_fault_stop '^(0xb|0x10001)$' "$(exception_code "$st")"
  check_stop D5_fault_line "$st" $D/threads.c fault_line
  local b_tid=$(field "$st" selected_thread) a_tid="" t
  for t in $(thread_ids "$st"); do
    [[ $t == $b_tid ]] && continue
    ipc "select_thread --thread:$t" >/dev/null
    if call_stack_symbols "$(ipc state)" | grep -q spinner; then a_tid=$t; break; fi
  done
  check_match D5_spinner_thread_found '^[0-9]+$' "$a_tid"
  ipc "select_thread --thread:$a_tid" >/dev/null
  st=$(go_cmd step_over)
  note "D5 after stepping the spinner: processes=$(process_count "$st") selected=$(field "$st" selected_thread) expl=$(stop_explanation "$st") code=$(exception_code "$st") line=$(stop_file_line "$st")"
  check D5_step_keeps_process 1 "$(process_count "$st")"
  check D5_step_stopped_on_spinner "$a_tid" "$(field "$st" selected_thread)"
  st=$(go_cmd run)
  note "D5 after continue: processes=$(process_count "$st") selected=$(field "$st" selected_thread) expl=$(stop_explanation "$st") code=$(exception_code "$st") exit=$(field "$st" last_exit_code)"
  check D5_second_stop_on_faulter "$b_tid" "$(field "$st" selected_thread)"
  check D5_process_alive 1 "$(process_count "$st")"
  check_match D5_second_stop_is_fault '^(0xb|0x10001)$' "$(exception_code "$st")"
  ipc kill_all >/dev/null; wait_exit >/dev/null
  finish
}

scenarios+=(D6_exec_echo)
scenario_D6_exec_echo() {
  begin D6_exec_echo $B/fork_exec_O0 exec_echo || return
  bp_tag $D/fork_exec.c before_exec_echo >/dev/null
  local st=$(run_stop); check_stop before_exec "$st" $D/fork_exec.c before_exec_echo
  local before=$(field "$st" stop_count)
  ipc run >/dev/null
  st=$(wait_exit_or_stop $before)
  if [[ "$(process_count "$st")" != "0" ]]; then
    note "D6 stop after exec: expl=$(stop_explanation "$st") code=$(exception_code "$st") module=$(stop_module "$st") modules: $(module_names "$st" | tr ' ' '\n' | sed 's|.*/||' | tr '\n' ' ')"
    check D6_modules_switched_to_echo 1 "$(module_names "$st" | grep -c '/bin/echo')"
    check D6_old_image_gone 0 "$(module_names "$st" | grep -c fork_exec)"
    ipc run >/dev/null; st=$(wait_exit)
  else
    note "D6: no stop after the exec; the process ran to its end"
  fi
  check D6_exit_code 0 "$(field "$st" last_exit_code)"
  note "D6: an exit code of 100+n means execl failed with errno n (EPERM 1, EACCES 13, ENOENT 2)"
  check D6_process_gone 0 "$(process_count "$st")"
  check D6_hi_printed 1 "$(grep -c '^hi$' $S/raddbg_stdout.txt)"
  note "debuggee output: $(grep -i 'exec' $S/raddbg_stdout.txt | head -2 | tr '\n' ' ')"
  finish
}

scenarios+=(D7_thread_exit_list)
scenario_D7_thread_exit_list() {
  begin D7_thread_exit_list $B/threads_O0 exit_then_bp || return
  bp_tag $D/threads.c worker_body >/dev/null
  local st=$(run_stop); check_stop D7_bp "$st" $D/threads.c worker_body
  check D7_stop_selects_thread_y 1 "$(call_stack_symbols "$st" | grep -c worker_body)"
  check D7_idx 7 "$(ev idx)"
  check D7_thread_list_main_and_y_only 2 "$(thread_count "$st")"
  note "D7 threads at the stop: $(thread_ids "$st") NewThread=$(event_count NewThread) EndThread=$(event_count EndThread)"
  finish
}

scenarios+=(D8_watchpoint_condition)
scenario_D8_watchpoint_condition() {
  begin D8_watchpoint_condition $B/basic_O0 || return
  local loop_bp=$(bp_tag $D/basic.c main_loop)
  local st=$(run_stop); check_stop at_main_loop "$st" $D/basic.c main_loop
  ipc "break_when_value_changes g_watched" >/dev/null
  st=$(ipc state)
  local wid=$(bp_ids "$st" | tr ' ' '\n' | grep -v "^\\$loop_bp\$" | grep . | head -1)
  check_match D8_watch_id '^\$[0-9a-f]+$' "$wid"
  ipc "name_cfg --cfg:$wid --string:watch_label" >/dev/null
  ipc "condition_cfg --cfg:$wid --string:g_watched==3" >/dev/null
  st=$(run_stop)
  check D8_condition_honored_value 3 "$(ev g_watched)"
  check D8_condition_honored_i 2 "$(ev i)"
  check D8_stop_names_watchpoint "$wid" "$(stop_bp_id "$st")"
  check D8_hit_count 1 "$(bp_hit_count "$st" "$wid")"
  note "D8 explanation: $(stop_explanation "$st")"
  ipc clear_breakpoints >/dev/null
  ipc run >/dev/null; st=$(wait_exit); check exit_code 42 "$(field "$st" last_exit_code)"
  finish
}

scenarios+=(D9_attach_kill9)
scenario_D9_attach_kill9() {
  scenario=D9_attach_kill9; echo "--- $scenario"
  $B/basic_O0 spin >/dev/null 2>&1 &
  local tpid=$!
  start_debugger "$(project D9_attach_kill9 $B/basic_O0 spin)" >/dev/null
  local st=$(go_cmd "attach --pid:$tpid")
  check D9_attached "$tpid" "$(process_ids "$st" | awk '{print $1}')"
  ipc run >/dev/null; wait_running
  local before=$(field "$st" stop_count)
  local t0=$(date +%s%N)
  kill -9 $tpid
  # PT_ATTACHEXC reports the SIGKILL as a soft signal and mac_sigkill breaks by default (as
  # linux_sigkill does): the first report is a stop; the exit follows the continue
  st=$(wait_stop ${before:-0})
  local stop_ms=$(( ($(date +%s%N) - t0) / 1000000 ))
  check D9_sigkill_stop 0x9 "$(exception_code "$st")"
  check D9_sigkill_stop_process_known 1 "$(process_count "$st")"
  t0=$(date +%s%N)
  ipc run >/dev/null
  for i in $(seq 1 100); do st=$(ipc state); [[ "$(process_count "$st")" == "0" ]] && break; done
  local ms=$(( ($(date +%s%N) - t0) / 1000000 ))
  check D9_exit_reported 0 "$(process_count "$st")"
  check D9_exit_code_is_signal 9 "$(field "$st" last_exit_code)"
  check_match D9_exit_within_500ms '^([0-9]|[1-9][0-9]|[1-4][0-9][0-9])$' "$ms"
  note "D9 SIGKILL stop after ${stop_ms} ms (100 ms polls), exit reported ${ms} ms after the continue (one ipc state round trip is ~50 ms)"
  wait_gone $tpid
  finish
}

# ===============================================================================================
# scenarios: unwinder review (U*)
# ===============================================================================================

scenarios+=(U1_zig_unwind)
scenario_U1_zig_unwind() {
  if [[ ! -x $GEN || ! -f $GSRC ]]; then scenario=U1_zig_unwind; note "generator not built; skipped"; return; fi
  begin U1_zig_unwind $GEN || return
  ipc "add_breakpoint --file_path:$GSRC:57" >/dev/null
  local st=$(run_stop); st=$(go_cmd step_into)
  check_match U1_in_next 'next' "$(stop_symbol "$st")"
  check_match U1_frame1_main_after_step_into 'main' "$(concrete_frames "$st" | sed -n 2p)"
  note "U1 stack after step into: $(call_stack "$st" | tr '\n' ' ')"
  # instruction-step through next() (up to its ret); frame 1 must stay main at every step
  local bad="" i steps=0
  for i in $(seq 1 120); do
    st=$(go_cmd step_into_inst); steps=$i
    echo "$(stop_symbol "$st")" | grep -q next || break
    if ! concrete_frames "$st" | sed -n 2p | grep -q main; then bad="pc=$(field "$st" ip) frames: $(concrete_frames "$st" | tr '\n' ' ')"; break; fi
  done
  check U1_frame1_main_at_every_instruction_of_next "" "$bad"
  note "U1 instruction steps in next: $steps, left next at $(stop_symbol "$st") $(stop_file_line "$st")"
  # step out right after the step into lands on the line after rng.next()
  st=$(restart_stop); st=$(go_cmd step_into); st=$(go_cmd step_out)
  check_match U1_step_out_after_step_into 'main.zig:(57|58)' "$(stop_file_line "$st")"
  check U1_step_out_sym "main" "$(stop_symbol "$st" | sed 's/.*\.//')"
  finish
}

scenarios+=(U2_noreturn_stack)
scenario_U2_noreturn_stack() {
  begin U2_noreturn_stack $B/noreturn_O2 || return
  local st=$(run_stop)
  # the trap's first chance, EXC_BREAKPOINT (mac_exc_breakpoint breaks by default)
  check U2_trap_stop 0x10006 "$(exception_code "$st")"
  check U2_top_frame "die" "$(concrete_frames "$st" | head -1)"
  check_match U2_stack_has_a_func 'a_func' "$(call_stack_symbols "$st")"
  check U2_stack_without_b_func 0 "$(call_stack_symbols "$st" | tr ' ' '\n' | grep -c '^b_func$')"
  check U2_frames_pc_symbol_names_b_func 1 "$(frame_ip_symbols "$st" | tr ' ' '\n' | grep -c '^b_func$')"
  note "U2 frames (pc-1 symbols): $(call_stack_symbols "$st") | pc symbols: $(frame_ip_symbols "$st")"
  finish
}

scenarios+=(U3_thread_sleep_stack)
scenario_U3_thread_sleep_stack() {
  begin U3_thread_sleep_stack $B/threads_O0 sleeper || return
  bp_tag $D/threads.c main_spin >/dev/null
  local st=$(run_stop); check two_threads 2 "$(thread_count "$st")"
  local main_tid=$(field "$st" selected_thread)
  local y=$(thread_ids "$st" | tr ' ' '\n' | grep -v "^$main_tid$" | grep . | head -1)
  ipc "select_thread --thread:$y" >/dev/null
  st=$(ipc state)
  check U3_selected_sleeper "$y" "$(field "$st" selected_thread)"
  local n=$(call_stack "$st" | grep -c .)
  check_match U3_frames_nonempty '^([3-9]|[1-9][0-9])$' "$n"
  check_match U3_top_in_libsystem 'libsystem_(kernel|c).dylib' "$(frame_modules "$st" | head -1)"
  check_match U3_sleeper_frame 'sleeper' "$(call_stack_symbols "$st")"
  check U3_bottom_libsystem_pthread "libsystem_pthread.dylib" "$(frame_modules "$st" | tail -1)"
  note "U3 sleeper stack: $(call_stack "$st" | tr '\n' ' ') modules: $(frame_modules "$st" | tr '\n' ' ')"
  ipc kill_all >/dev/null; wait_exit >/dev/null
  finish
}

scenarios+=(U5_tls_step_into)
scenario_U5_tls_step_into() {
  begin U5_tls_step_into $B/basic_O0 || return
  bp_tag $D/basic.c main_tls >/dev/null
  local st=$(run_stop); check_stop at_main_tls "$st" $D/basic.c main_tls
  st=$(go_cmd step_into)
  note "U5 step into from the TLS line: module=$(stop_module "$st") sym=$(stop_symbol "$st") line=$(stop_file_line "$st") stack: $(call_stack "$st" | tr '\n' ' ') modules: $(frame_modules "$st" | tr '\n' ' ')"
  if [[ "$(stop_symbol "$st")" == "main" ]]; then
    check U5_stayed_in_main "$(tag_loc $D/basic.c main_static)" "$(stop_file_line "$st")"
  else
    check U5_frame1_is_main "main" "$(concrete_frames "$st" | sed -n 2p)"
    st=$(go_cmd step_out)
    # the getter returns into the middle of the TLS line (its store follows the call), so a step
    # out lands on that line; one step over then reaches the next line
    check_match U5_step_out_returns_to_caller "basic.c:($(line_of $D/basic.c main_tls)|$(line_of $D/basic.c main_static))" "$(stop_file_line "$st")"
    check U5_step_out_sym "main" "$(stop_symbol "$st")"
    [[ "$(stop_file_line "$st")" == "$(tag_loc $D/basic.c main_tls)" ]] && st=$(go_cmd step_over)
    check_stop U5_next_line "$st" $D/basic.c main_static
  fi
  if [[ "$(stop_file_line "$st")" == "$(tag_loc $D/basic.c main_static)" ]]; then
    check tls_counter_after_access 8 "$(ev t_tls_counter)"
  fi
  finish
}

# ===============================================================================================
# main
# ===============================================================================================
if [[ "$1" == "-l" ]]; then printf '%s\n' "${scenarios[@]}"; exit 0; fi
selected=("$@"); [[ ${#selected} -eq 0 ]] && selected=("${scenarios[@]}")
echo "raddbg: $RADDBG"; echo "debuggees: $B"; echo "harness dir: $HARNESS_DIR"
start_time=$(date +%s)
for sc in "${selected[@]}"; do
  if typeset -f scenario_$sc >/dev/null; then scenario_$sc; else echo "unknown scenario: $sc"; fi
done
kill_test_instances
echo
echo "=== $pass_count passed, $fail_count failed in $(( $(date +%s) - start_time )) s"
for f in "${failures[@]}"; do echo "  FAIL $f"; done
exit $(( fail_count > 0 ))
