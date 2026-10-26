#!/bin/zsh
# A hardware watchpoint is per thread (DBGWVR/DBGWCR): a thread created after the watchpoint was armed
# must get it when the demon first sees it, or its writes never stop. The demon meets a new thread at
# a stop (Mach has no thread-creation notification): here the passed SIGUSR1 main raises after creating
# the writer (its stop is turned off, so the engine resumes at once); the writer starts writing after
# that. (Threads seen at an internal round, one without an event for the engine, are programmed as they
# are allocated; a thread created and writing within one uninterrupted run is not caught.)
source "$(dirname ${(%):-%x})/common.sh"
build_c watch_thread
write_project watch_thread $S/watch_thread "" <<'EOF'
mac_sigusr1: 0
breakpoint:
{
  address_location: "(uint64)p"
  address_range_size: 4
  break_on_write: 1
  enabled: 1
}
EOF
launch $S/watch_thread.raddbg
bp $DEMON_TESTS_DIR/src/watch_thread.c:$(line_of watch_thread "// bp_setup")
go
st=$(ipc state)
check "stopped where the pointer is set (one thread)" '[[ $(stop_line "$st") == $(line_of watch_thread "// bp_setup") && $(thread_count "$st") == 1 ]]'
go
st=$(ipc state)
wl=$(line_of watch_thread '// write')
check "stopped at the writer thread's write (line $wl) or right after it (reported: $(stop_line "$st"))" '[[ $(stop_line "$st") == $wl || $(stop_line "$st") == $((wl+1)) ]]'
check "the stop is a breakpoint hit at the written address" '[[ $(stop_explanation "$st") == *"hit a breakpoint (Address: 0x"* ]]'
check "the stop is in the writer thread, created during the run" '[[ $(stop_symbol "$st") == writer && $(thread_count "$st") == 2 ]]'
check "the process ran to its end without the watchpoint" 'ipc clear_breakpoints >/dev/null; ipc run >/dev/null; wait_no_threads 50 && grep -q "^done 50" $S/watch_thread.stdout.txt'
finish watch_new_thread
