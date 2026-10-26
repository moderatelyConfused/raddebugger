#!/bin/zsh
# A reported thread which exits before another (new) thread stops the process must be reported as
# exited: it is absent from the thread list at the next stop.
# (threads are told apart by id: thread names arrive with the run after the stop which gathered them)
source "$(dirname ${(%):-%x})/common.sh"
build_c thread_exit
write_project thread_exit $S/thread_exit ""
launch $S/thread_exit.raddbg
st0=$(ipc state)
main_id=$(thread_ids "$st0" | head -1)
bp $DEMON_TESTS_DIR/src/thread_exit.c:$(line_of thread_exit "// bp_main")
bp $DEMON_TESTS_DIR/src/thread_exit.c:$(line_of thread_exit "// bp_y")
go
st1=$(ipc state)
x_id=$(thread_ids "$st1" | grep -vx "$main_id" | head -1)
check "two threads at the first stop (main $main_id and thread_x $x_id)" '[[ $(thread_count "$st1") == 2 && -n $x_id ]]'
go
st2=$(ipc state)
check "the second stop is thread_y's breakpoint" '[[ $(stop_symbol "$st2") == y_func ]]'
check "thread_x ($x_id) is gone from the thread list" '! thread_ids "$st2" | grep -qx "$x_id"'
check "two threads at the second stop (main and thread_y): $(thread_ids "$st2" | tr '\n' ' ')" '[[ $(thread_count "$st2") == 2 ]]'
check "an ExitThread event was reported" '[[ $(log_events ExitThread) -ge 1 ]]'
finish thread_exit
