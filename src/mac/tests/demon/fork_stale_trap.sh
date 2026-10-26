#!/bin/zsh
# A forked child which hits a trap written by an earlier run: the child is forked while the breakpoint
# on foo() is in place (the run writes the trap before the fork; the child's copy-on-write page keeps
# the brk when the parent's copy is restored at the run's end), the breakpoint is then removed and the
# child, which slept through all that, calls foo(). The demon must put the original instruction back
# from its record of the traps ever written into the parent; before, only the run in progress was
# known and the child took SIGTRAP (the parent then saw a signalled child, status 5).
source "$(dirname ${(%):-%x})/common.sh"
build_c fork_child
write_project fork_child $S/fork_child "stale" </dev/null
launch $S/fork_child.raddbg
bp $DEMON_TESTS_DIR/src/fork_child.c:$(line_of fork_child "// bp_foo")
bp $DEMON_TESTS_DIR/src/fork_child.c:$(line_of fork_child "// bp_after_fork")
go
st=$(ipc state)
check "the parent stopped right after the fork (the child is sleeping with the inherited trap)" '[[ $(stop_line "$st") == $(line_of fork_child "// bp_after_fork") ]]'
ipc clear_breakpoints >/dev/null
ipc run >/dev/null
wait_no_threads 50; r=$?
check "the parent ran to its exit" '[[ $r == 0 ]]'
check "the child ran foo after the breakpoint was removed" 'grep -q "child: foo ran" $S/fork_child.stdout.txt'
check "the demon restored the trap of the earlier run in the child" 'grep -q "hit an inherited trap .*restored the original instruction" $CTRL_LOG'
check "no fork_child process is spinning or stuck" '! ps -axo pcpu,comm | awk "\$1 > 50 && \$2 ~ /fork_child/" | grep -q .'
echo "  INFO  parent: $(grep 'waitpid ->' $S/fork_child.stdout.txt)"
check "the parent observed the child's exit, or EINTR (a traced process is woken for every signal, SIGCHLD included: see the README)" 'grep -q "waitpid -> .*code 7\|waitpid -> -1 errno 4" $S/fork_child.stdout.txt'
finish fork_stale_trap
