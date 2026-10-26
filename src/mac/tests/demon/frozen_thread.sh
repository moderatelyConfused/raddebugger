#!/bin/zsh
# A frozen thread keeps its pending exception until it runs again, instead of having it answered with
# the ignore flag of a run which is about some other thread's event.
# `select_thread` cannot be driven over IPC (entity slots are not filled from option strings), so the
# situation is arranged through the event order instead: three threads raise at once, the faulting
# one first (its exception is the stop: EXC_BAD_ACCESS breaks by default), then a thread hitting a
# breakpoint, then a thread raising a signal the engine passes through (SIGUSR1 breaks by default
# too, so the project turns `mac_sigusr1` off; the `mac_*` names come from MAC_ExceptionCodeTable).
# A step then makes the engine single-step the breakpoint thread past its trap in a run with
# `ignore_previous_exception: 0`, the fault thread frozen with its reported exception pending: before
# the fix that reply escalated the fault to SIGSEGV and the process died when continued; now the
# continue stops on the fault again. (the three raises land in one round when the threads are
# released together; the log lines checked below say whether they did.)
source "$(dirname ${(%):-%x})/common.sh"
build_c frozen_thread
write_project frozen_thread $S/frozen_thread "" <<'EOF'
mac_sigusr1: 0
EOF
# rjf: runs so far whose single step had another thread frozen: `single_step_thread: [0x...]` with a nonzero handle, ignore flag 0
ignore0_step_runs() { awk '/single_step_thread:/ {s=$2} /ignore_previous_exception:/ {if(s != "[0x0]" && $2 == "0") n++} END {print n+0}' $CTRL_LOG 2>/dev/null; }
launch $S/frozen_thread.raddbg
bp $DEMON_TESTS_DIR/src/frozen_thread.c:$(line_of frozen_thread "go = 1;")
bp $DEMON_TESTS_DIR/src/frozen_thread.c:$(line_of frozen_thread "// bp_func")
go
st=$(ipc state)
check "stopped in main before releasing the threads (3 threads created)" "[[ $(thread_count "$st") == 4 ]]"
go
st1=$(ipc state)
ip1=$(field "$st1" ip)
check "first stop: fault_thread's exception" "echo '$st1' | grep -q 'ip_voff_symbol: \"fault_thread\"' && echo '$st1' | grep -q 'hit an exception'"
exc1=$(log_events Exception) bp1=$(log_events Breakpoint) steps1=$(ignore0_step_runs)
echo "  INFO  the round reported $exc1 exception(s) (fault, SIGUSR1) and $bp1 breakpoint hit(s) so far"
step step_over 1
steps2=$(ignore0_step_runs)
check "the step single-stepped the breakpoint thread past its trap with ignore 0, the fault thread frozen ($((steps2-steps1)) such run(s))" "[[ $steps2 -gt $steps1 ]]"
go
st3=$(ipc state)
check "continuing stops on fault_thread's fault again (ip $ip1), the process is alive" "[[ \"$(field "$st3" ip)\" == \"$ip1\" ]] && echo '$st3' | grep -q 'fault_thread' && [[ $(log_events ExitProcess) == 0 ]]"
finish frozen_thread
