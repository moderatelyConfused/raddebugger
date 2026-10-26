#!/bin/zsh
# A hard exception the engine passes (EXC_BAD_ACCESS and SIGSEGV stops disabled) must be delivered as
# its signal: the SIGSEGV handler runs and recovers, the program exits normally.
# Exception stops are configured through the `mac_*` settings read by the UI (names from
# MAC_ExceptionCodeTable in src/mac/mac.mdesk, prefixed with mac_; raddbg.mdesk's project schema gives
# them the table's defaults, which break on EXC_BAD_ACCESS, SIGSEGV and SIGBUS), so the pass-through
# this check is about is a non-default: the project turns the three off. (The default path, a first
# chance stop whose continue delivers the signal to the handler, is the flow suite's `D4`.)
source "$(dirname ${(%):-%x})/common.sh"
build_c sigsegv_handler
write_project sigsegv_handler $S/sigsegv_handler "" <<'EOF'
mac_exc_bad_access: 0
mac_sigsegv: 0
mac_sigbus: 0
EOF
launch $S/sigsegv_handler.raddbg
ipc run >/dev/null
wait_no_threads 50; r=$?
check "the process ran to its exit" '[[ $r == 0 ]]'
check "the SIGSEGV handler ran ('recovered' printed)" 'grep -q "^recovered$" $S/sigsegv_handler.stdout.txt'
check "one Exception event (the first chance), the escalated signal was not re-reported" '[[ $(log_events Exception) == 1 ]]'
check "one ExitProcess event" '[[ $(log_events ExitProcess) == 1 ]]'
finish signal_handler
