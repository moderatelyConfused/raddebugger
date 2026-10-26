#!/bin/zsh
# Several threads' exceptions in one round, with the engine's ignore flag applying to one at a time.
# `select_thread` cannot be driven over IPC (entity slots are not filled from option strings), so the
# situation is arranged through the event order instead: three threads raise at once, the faulting
# one first (its exception is the stop: EXC_BAD_ACCESS breaks by default), then a thread hitting a
# breakpoint, then a thread raising a signal the engine passes through (SIGUSR1 breaks by default
# too, so the project turns `mac_sigusr1` off; the `mac_*` names come from MAC_ExceptionCodeTable).
# What must happen: the fault is the first stop; the breakpoint thread's hit, held back by that stop,
# is the next (the engine reports it on the next run instead of stepping the thread off its trap);
# the SIGUSR1 exception, held back by the demon (a run hands the engine one exception event), is
# reported after that, passed by the engine and delivered to its thread: its handler writes a line.
# A continue then stops on the fault again (the engine's continue after an exception stop drops the
# exception: the instruction re-executes), the process alive. Before the demon fixes, the run which
# resumed the SIGUSR1 thread carried the fault stop's "ignore" and the signal was dropped, or the
# fault's pending exception was answered with a passing run's flag and the process died of SIGSEGV.
source "$(dirname ${(%):-%x})/common.sh"
build_c frozen_thread
write_project frozen_thread $S/frozen_thread "" <<'EOF'
mac_sigusr1: 0
EOF
launch $S/frozen_thread.raddbg
bp $DEMON_TESTS_DIR/src/frozen_thread.c:$(line_of frozen_thread "go = 1;")
bp $DEMON_TESTS_DIR/src/frozen_thread.c:$(line_of frozen_thread "// bp_func")
go
st=$(ipc state)
check "stopped in main before releasing the threads (3 threads created)" '[[ $(thread_count "$st") == 4 ]]'
go
st1=$(ipc state)
ip1=$(field "$st1" ip)
check "first stop: fault_thread's exception" '[[ $(stop_symbol "$st1") == fault_thread && $(stop_explanation "$st1") == *"hit an exception"* ]]'
exc1=$(log_events Exception) bp1=$(log_events Breakpoint) held1=$(grep -c 'holding back the exception' $CTRL_LOG 2>/dev/null)
echo "  INFO  the round reported $exc1 exception event(s) and $bp1 breakpoint hit(s); the demon held back ${held1:-0} exception(s)"
step step_over 1
st2=$(ipc state)
held=$(grep -c 'held_back_breakpoint_hit: .* reported now' $CTRL_LOG 2>/dev/null)
check "the step reported the breakpoint thread's held-back hit (${held:-0} reported), the fault thread frozen" '[[ ${held:-0} -gt 0 && $(stop_symbol "$st2") == bp_func && $(stop_explanation "$st2") == *"hit a breakpoint"* ]]'
go
st3=$(ipc state)
check "continuing stops on fault_thread's fault again (ip $ip1), the process is alive" '[[ "$(field "$st3" ip)" == "$ip1" && $(stop_symbol "$st3") == fault_thread && $(log_events ExitProcess) == 0 ]]'
# rjf: the SIGUSR1 thread was resumed with its passed signal; its handler needs a little running time,
# which each re-fault stop cuts short, so give it a few more rounds
for i in $(seq 1 20); do grep -q '^usr1 handled$' $S/frozen_thread.stdout.txt 2>/dev/null && break; go >/dev/null; done
check "the passed SIGUSR1 reached its handler ('usr1 handled' printed; $(grep -c 'held back from an earlier round' $CTRL_LOG 2>/dev/null) held exception(s) reported later)" 'grep -q "^usr1 handled$" $S/frozen_thread.stdout.txt'
check "the process is still alive at the end" '[[ $(log_events ExitProcess) == 0 && $(thread_count "$(ipc state)") == 4 ]]'
finish frozen_thread
