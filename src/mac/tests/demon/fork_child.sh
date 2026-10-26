#!/bin/zsh
# Forked children inherit the exception ports and the breakpoint instructions: the child must not spin
# on an inherited brk; with the original instruction restored it runs the function normally.
source "$(dirname ${(%):-%x})/common.sh"
build_c fork_child
write_project fork_child $S/fork_child "" </dev/null
launch $S/fork_child.raddbg
bp $DEMON_TESTS_DIR/src/fork_child.c:$(line_of fork_child "// bp_foo")
bp $DEMON_TESTS_DIR/src/fork_child.c:$(line_of fork_child "// bp_after_wait")
go
st=$(ipc state)
check "the parent reached the line after wait()" "echo '$st' | grep -q 'line_num: *$(line_of fork_child '// bp_after_wait')\$'"
check "no fork_child process is spinning" "! ps -axo pcpu,comm | awk '\$1 > 50 && \$2 ~ /fork_child/' | grep -q ."
check "the child ran foo (inherited trap restored)" "grep -q 'child: foo ran' $S/fork_child.stdout.txt"
check "the demon logged the inherited trap" "grep -q 'hit an inherited trap' $CTRL_LOG"
ipc run >/dev/null
wait_no_threads 30
check "the parent saw a clean child exit" "grep -q 'child status 0' $S/fork_child.stdout.txt"
finish fork_child
