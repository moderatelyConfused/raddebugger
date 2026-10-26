#!/bin/zsh
# Runs every macOS demon check (see common.sh for the settings). Each check starts its own debugger
# instance on IPC_PORT and leaves its artifacts (debuggees, project files, stdout files, logs) in HARNESS_DIR.
cd "$(dirname ${(%):-%x})"
export HARNESS_DIR=${HARNESS_DIR:-$(mktemp -d /tmp/raddbg_demon_tests.XXXXXX)}
echo "artifacts in $HARNESS_DIR"
failed=0
for t in detach fork_child signal_handler frozen_thread thread_exit exec watchpoint; do
  echo "#### $t"
  zsh ./$t.sh || failed=$((failed+1))
done
echo "####"
if [[ $failed == 0 ]]; then echo "all demon checks passed"; else echo "$failed demon check script(s) failed"; fi
exit $failed
