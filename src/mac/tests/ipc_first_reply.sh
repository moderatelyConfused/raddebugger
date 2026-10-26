#!/bin/zsh
# Proof for the "first --ipc reply stalls 10s" defect (src/mac/README.md, open issues).
#
# Launches a fresh raddbg instance on a private port, times the FIRST `state`
# command sent to it (nothing is sent before it, so it is genuinely the first
# connection), kills the instance by pid, and repeats. PASS requires every first
# command to answer in under one second.
#
# Settings: RADDBG (binary; default build/raddbg under the repo root),
# IPC_PORT (default 7601, must be in 7600-7699), TRIALS (default 10),
# HARNESS_DIR (scratch for the private user file / logs; lib.sh).
#
# This proof starts its instance by hand and never through lib.sh's dbg_start / wait_up: nothing may
# be sent to the instance before the timed first command (wait_up probes with `state`). It takes from
# lib.sh only what sends nothing: the binary, the scratch directory, the listener readiness
# (wait_listener: lsof, LISTEN on the port, no connection) and the kill by listener pid.
set -u
export IPC_PORT=${IPC_PORT:-7601}
source "$(cd "$(dirname ${(%):-%x})" && pwd)/lib.sh"
# a scratch directory of our own is removed at exit; a HARNESS_DIR given by the caller is theirs
remove_owned_harness_dir_at_exit
N=${TRIALS:-10}
LIMIT=${LIMIT:-1.0}
now() { python3 -c 'import time;print(time.time())'; }

fails=0
for trial in $(seq 1 $N); do
  kill_test_instances
  rm -f $S/u.raddbg_user; rm -rf $S/logs
  ($RD $P --user:$S/u.raddbg_user --project:$S/p.raddbg --logs:$S/logs > $S/stdout.txt 2>&1 &)
  if ! wait_listener >/dev/null; then echo "trial $trial: instance never listened on $IPC_PORT"; fails=$((fails+1)); continue; fi
  PID=$RD_PID
  t0=$(now); out=$($RD $P --ipc state 2>&1); t1=$(now)
  dt=$(python3 -c "print(round($t1-$t0,3))")
  ok=$(python3 -c "print('OK' if $t1-$t0 < $LIMIT else 'SLOW')")
  lines=$(echo "$out" | grep -c .)
  echo "trial $trial (pid $PID): first state = ${dt}s  [$ok]  (${lines} reply lines)"
  [[ "$ok" == "OK" && "$lines" -gt 1 ]] || fails=$((fails+1))
  kill -9 $PID 2>/dev/null; sleep 0.1
done
kill_test_instances
echo "----"
if [[ $fails -eq 0 ]]; then echo "PASS: all $N first commands answered in < ${LIMIT}s"; exit 0
else echo "FAIL: $fails / $N trials failed"; exit 1; fi
