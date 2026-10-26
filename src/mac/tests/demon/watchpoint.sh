#!/bin/zsh
# A data breakpoint (hardware watchpoint) stop is attributed to its breakpoint: the demon reports the
# trap id in the event's user_data, which is how the engine identifies the breakpoint that was hit
# (the UI then counts the hit: `hit_count` in the autosaved project file).
# The breakpoint's address expression is a local pointer, evaluated when a run starts: the first run
# (from the entry point) stops at a line where it is set, the second run arms the watchpoint.
# (IPC output suppresses addresses, so the pointer's value cannot be checked through `eval`.)
source "$(dirname ${(%):-%x})/common.sh"
build_c watch_global
write_project watch_global $S/watch_global "" <<'EOF'
breakpoint:
{
  address_location: "p"
  address_range_size: 4
  break_on_write: 1
  enabled: 1
}
EOF
launch $S/watch_global.raddbg
bp $DEMON_TESTS_DIR/src/watch_global.c:$(line_of watch_global "// bp_setup")
go
st=$(ipc state)
check "stopped where the pointer is set" "echo '$st' | grep -q 'line_num: *$(line_of watch_global '// bp_setup')\$'"
go
st=$(ipc state)
check "stopped at the write (line $(line_of watch_global '// write'))" "echo '$st' | grep -q 'line_num: *$(line_of watch_global '// write')\$'"
check "the stop is a breakpoint hit at the written address" "echo '$st' | grep -q 'hit a breakpoint (Address: 0x'"
# rjf: save the project (to a new path: saving over an existing file asks for a confirmation the
# script cannot give) to read the hit count. NOTE: the path option must be given, with a `:line`
# suffix (a FilePath register is only filled from a path:line string): without it `save_project`
# over IPC takes the focused tab's file path, i.e. the source file, as its destination.
SAVED=$S/watch_global.saved.raddbg
rm -f $SAVED
ipc "save_project --file_path:$SAVED:1" >/dev/null
for i in $(seq 1 30); do grep -q "hit_count: *1" $SAVED 2>/dev/null && break; sleep 0.1; done
check "the data breakpoint's hit count became 1 (the hit was attributed to it)" "grep -q 'hit_count: *1' $SAVED"
finish watchpoint
