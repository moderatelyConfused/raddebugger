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
# HARNESS_DIR (scratch for the private user file / logs).
set -u
RD=${RADDBG:-"$(cd "$(dirname ${(%):-%x})/../../.." && pwd)/build/raddbg"}
P=${IPC_PORT:-7601}
N=${TRIALS:-10}
S=${HARNESS_DIR:-$(mktemp -d /tmp/raddbg_ipc_first_reply.XXXXXX)}
LIMIT=${LIMIT:-1.0}
mkdir -p $S
now() { python3 -c 'import time;print(time.time())'; }
kill_port() { pgrep -f "raddbg --ipc_port:$P" | xargs kill -9 2>/dev/null; for i in $(seq 1 100); do pgrep -f "raddbg --ipc_port:$P" >/dev/null || break; sleep 0.02; done; }
# readiness via lsof (LISTEN) instead of nc -z: nc would itself be the first
# connection and absorb the very bug under test.
wait_listen() { for i in $(seq 1 600); do lsof -nP -iTCP:$P -sTCP:LISTEN 2>/dev/null | grep -q raddbg && return 0; sleep 0.05; done; return 1; }

fails=0
for trial in $(seq 1 $N); do
  kill_port
  rm -f $S/u.raddbg_user; rm -rf $S/logs
  ($RD --ipc_port:$P --user:$S/u.raddbg_user --project:$S/p.raddbg --logs:$S/logs > $S/stdout.txt 2>&1 &)
  if ! wait_listen; then echo "trial $trial: instance never listened on $P"; fails=$((fails+1)); continue; fi
  PID=$(pgrep -f "raddbg --ipc_port:$P")
  t0=$(now); out=$($RD --ipc_port:$P --ipc state 2>&1); t1=$(now)
  dt=$(python3 -c "print(round($t1-$t0,3))")
  ok=$(python3 -c "print('OK' if $t1-$t0 < $LIMIT else 'SLOW')")
  lines=$(echo "$out" | grep -c .)
  echo "trial $trial (pid $PID): first state = ${dt}s  [$ok]  (${lines} reply lines)"
  [[ "$ok" == "OK" && "$lines" -gt 1 ]] || fails=$((fails+1))
  kill -9 $PID 2>/dev/null; sleep 0.1
done
kill_port
echo "----"
if [[ $fails -eq 0 ]]; then echo "PASS: all $N first commands answered in < ${LIMIT}s"; exit 0
else echo "FAIL: $fails / $N trials failed"; exit 1; fi
