#!/bin/zsh
# Detaching at a signal stop must leave the signal with the process: PT_DETACH resumes like PT_CONTINUE
# and its data (0) replaces the signal a thread blocked in a soft-signal exception takes, so before the
# fix the signal the process was stopped at was dropped: a SIGUSR1 handler never ran, a SIGTERM with
# the default disposition did not terminate the process. The demon now marks the signal pending on its
# thread (PT_THUPDATE) before PT_DETACH.
source "$(dirname ${(%):-%x})/common.sh"
build_c detach_signal

echo "== detach at a SIGUSR1 stop (handler installed)"
write_project detach_signal $S/detach_signal "" <<'EOF'
mac_sigusr1: 1
mac_sigterm: 1
EOF
launch $S/detach_signal.raddbg
go
st=$(ipc state)
pid=$(process_ids "$st" | head -1)
check "stopped at the SIGUSR1 exception" '[[ $(stop_explanation "$st") == *"SIGUSR1"* ]]'
ipc detach >/dev/null
wait_no_threads 20; r=$?
check "the debugger forgot the process" '[[ $r == 0 ]]'
wait_pid_gone $pid 50; r=$?
check "the detached process ran to its end" '[[ $r == 0 ]]'
check "the SIGUSR1 handler ran after the detach ('usr1 handled' printed)" 'grep -q "^usr1 handled$" $S/detach_signal.stdout.txt'
check "main continued ('after' printed)" 'grep -q "^after$" $S/detach_signal.stdout.txt'
check "the demon logged the signal it left pending" 'grep -q "detaching pid .* at its signal 30" $CTRL_LOG'

echo "== detach at a SIGTERM stop (default disposition: the process must die of it)"
write_project detach_signal $S/detach_signal "term" <<'EOF'
mac_sigusr1: 1
mac_sigterm: 1
EOF
launch $S/detach_signal.raddbg
go
st=$(ipc state)
pid=$(process_ids "$st" | head -1)
check "stopped at the SIGTERM exception" '[[ $(stop_explanation "$st") == *"SIGTERM"* ]]'
ipc detach >/dev/null
wait_no_threads 20
wait_pid_gone $pid 50; r=$?
check "the detached process is gone (terminated by the signal)" '[[ $r == 0 ]]'
check "'before' was printed, 'after' was not (SIGTERM terminated the process)" 'grep -q "^before$" $S/detach_signal.stdout.txt && ! grep -q "^after$" $S/detach_signal.stdout.txt'
finish detach_at_signal
