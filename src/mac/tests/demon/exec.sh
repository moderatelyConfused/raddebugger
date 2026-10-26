#!/bin/zsh
# exec: the debuggee replaces itself with another program; the new program's output appears, the exit
# is clean (the post-exec SIGTRAP is swallowed, not reported) and the module list switches to the new
# image.
# The exec target is a binary of our own (echo_hi). Exec'ing a platform binary such as /bin/echo
# kills the traced process with SIGTRAP on this OS: the exec resets the task's exception ports when
# the debugger lacks Apple's private task-port entitlement, so the SIGTRAP takes its default action
# (lldb behaves the same: "Terminated due to signal 5"); that case is run last, for information.
source "$(dirname ${(%):-%x})/common.sh"
build_c exec_echo
build_c echo_hi
write_project exec_echo $S/exec_echo "$S/echo_hi" </dev/null
launch $S/exec_echo.raddbg
st0=$(ipc state)
check "exec_echo is the main module before the exec" "module_names '$st0' | grep -q exec_echo"
ipc run >/dev/null
wait_no_threads 50; r=$?
check "the process ran to its exit" "[[ $r == 0 ]]"
check "'before exec' and 'hi' were printed" "grep -q '^before exec\$' $S/exec_echo.stdout.txt && grep -q '^hi\$' $S/exec_echo.stdout.txt"
check "no Exception event (the SIGTRAP was swallowed), one ExitProcess" "[[ $(log_events Exception) == 0 && $(log_events ExitProcess) == 1 ]]"
check "the demon noticed the exec" "grep -q \"exec'd; rediscovering\" $CTRL_LOG"
check "the old image was unloaded and echo_hi loaded" "grep -B12 'string: *\"'$S'/echo_hi\"' $CTRL_LOG | grep -q 'kind: *LoadModule' && [[ $(log_events UnloadModule) -ge 2 ]]"

echo "== for information: exec of the platform binary /bin/echo"
write_project exec_echo $S/exec_echo "" </dev/null
launch $S/exec_echo.raddbg
ipc run >/dev/null
wait_no_threads 50
if grep -q '^hi$' $S/exec_echo.stdout.txt; then echo "  INFO  /bin/echo ran after the exec"; else echo "  INFO  the process did not survive the exec of /bin/echo (expected on this OS; lldb: 'Terminated due to signal 5')"; fi
finish exec
