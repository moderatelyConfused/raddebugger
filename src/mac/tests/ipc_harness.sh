#!/bin/zsh
# IPC harness for driving the macOS debugger from a script (see src/mac/README.md, "Driving the
# debugger from a script"). Source it, then:
#   launch <project.raddbg>        start a private instance (its own IPC port, user file and logs), launch the
#                                  target and stop at its entry point, wait for the debug info
#   bp <abs path>:<line>           add a source breakpoint
#   go                             run to the next stop and print it
#   step <step_over|step_into|step_out|step_over_inst|step_into_inst> [n]
#                                  step n times, printing every stop as
#                                  "<voff> <symbol>  <unit line> > ... > <innermost inlined line>  (sel depth d) [why]"
#   ipc <command...>               any `raddbg --ipc` command against the instance
#   kill_test_instances            end the instance(s) on this harness' port
# Settings: RADDBG (binary, default build/raddbg under the repo root), IPC_PORT (default 7424),
# HARNESS_DIR (where the user file and logs go, default a temp dir).
# Gotchas: once a disassembly tab is focused (a stop without source), steps become instruction steps.
# fast IPC harness: no fixed sleeps, polls the debugger's stop counter instead.
# usage: source h.sh; launch <project.raddbg>; bp <path:line>; go; step step_over 3; ...
RD=${RADDBG:-"$(cd "$(dirname ${(%):-%x})/../../.." && pwd)/build/raddbg"}
P=--ipc_port:${IPC_PORT:-7424}
S=${HARNESS_DIR:-$(mktemp -d /tmp/raddbg_ipc_harness.XXXXXX)}
ipc() { $RD $P --ipc "$@" 2>&1; }
field() { echo "$1" | grep -m1 " $2:" | awk '{print $2}'; }
wait_up() { for i in $(seq 1 300); do ipc state | grep -q "version" && return 0; sleep 0.1; done; echo "debugger did not come up"; return 1; }
# wait until a new stop has been recorded (stop_count grew) and the thread is not running
# wait for a new stop (stop_count grew), or for the selected inline frame to change without a run (a step into
# at an inlined call's first instruction only selects the next frame); 100ms polls (each poll renders a frame), 8s cap
wait_stop() { local before=${1:-0} depth_before=${2:-x}; for i in $(seq 1 80); do st=$(ipc state); local sc=$(field "$st" stop_count); if [[ "$(field "$st" running)" == "0" && -n "$sc" ]]; then if [[ "$sc" -gt "$before" || "$(field "$st" inline_depth)" != "$depth_before" ]]; then echo "$st"; return 0; fi; fi; sleep 0.1; done; echo "$st"; return 1; }
wait_dbginfo() { for i in $(seq 1 60); do ipc state | sed -n '/^ lines:/,/^ }/p' | grep -q "file_name" && return 0; sleep 0.25; done; echo "debug info not ready (no line info at the stop)"; return 1; }
show() { local st=$1 tag=$2; local voff=$(field "$st" ip_voff) sym=$(echo "$st" | grep -m1 "ip_voff_symbol:" | cut -d'"' -f2) depth=$(field "$st" inline_depth) expl=$(echo "$st" | grep -m1 "explanation:" | cut -d'"' -f2); local lines=$(echo "$st" | sed -n '/^ lines:/,/^ threads:/p' | grep "file_name\|line_num" | paste - - | awk -F'"' '{split($3,a,":"); gsub(/ /,"",a[2]); printf "%s%s:%s", (NR>1?" > ":""), $2, a[2]}'); echo "$tag: $voff $sym  $lines  (sel depth $depth) [$expl]"; }
# run a command that makes the thread run, wait for the stop, print it
run_cmd() { local st0=$(ipc state); local before=$(field "$st0" stop_count); before=${before:-0}; local depth_before=$(field "$st0" inline_depth); ipc "$1" >/dev/null; show "$(wait_stop $before ${depth_before:-x})" "$2"; }
step() { for i in $(seq 1 ${2:-1}); do run_cmd "$1" "$i"; done; }
go() { run_cmd run run; }
bp() { ipc "add_breakpoint --file_path:$1" >/dev/null; }
kill_test_instances() { for pid in $(pgrep -f "raddbg $P"); do kill -9 $pid 2>/dev/null; done; for i in $(seq 1 100); do pgrep -f "raddbg $P" >/dev/null || break; sleep 0.05; done; }
launch() { kill_test_instances; mkdir -p $S; rm -f $S/test.raddbg_user; (nohup $RD $P --project:$1 --user:$S/test.raddbg_user --logs:$S/logs > $S/raddbg_stdout.txt 2>&1 &); for i in $(seq 1 100); do nc -z -G 1 127.0.0.1 ${P#--ipc_port:} >/dev/null 2>&1 && break; sleep 0.05; done; wait_up && launch_target && wait_dbginfo; }
# NOTE: the first IPC reply of a fresh instance is misdelivered (see README); the throwaway `state` above absorbs that 10s client timeout in the background
# step_into with no process launches the active target and stops at its entry (launch_and_step_into over IPC has no target cfg in its registers); retry until the stop is seen
launch_target() { for i in $(seq 1 20); do local before=$(field "$(ipc state)" stop_count); before=${before:-0}; ipc step_into >/dev/null; for j in $(seq 1 40); do st=$(ipc state); local sc=$(field "$st" stop_count); if [[ "$(field "$st" running)" == "0" && -n "$sc" && "$sc" -gt "$before" ]]; then show "$st" entry; return 0; fi; sleep 0.05; done; done; echo "launch failed"; return 1; }
