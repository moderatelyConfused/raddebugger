#!/bin/zsh
# Proof for `raddbg --ipc --pid:<pid>` routing (src/raddbg/tests/raddbg_tests.c
# drives the debugger this way). Each instance listens, in addition to its default
# IPC port, on a port derived from its own pid (rd_ipc_port_from_pid in
# raddbg_main.c); a client with --pid:<pid> derives the same port, so the command
# reaches that specific instance.
#
# This launches two instances on two distinct in-range ports, confirms each
# listens on its own pid-derived port (and not the other's), drives each by --pid,
# and checks the default-port path still works. Windows appear briefly and are
# killed by pid.
#
# Settings: RADDBG, PORT_A (7603), PORT_B (7604), HARNESS_DIR.
set -u
RD=${RADDBG:-"$(cd "$(dirname ${(%):-%x})/../../.." && pwd)/build/raddbg"}
PA=${PORT_A:-7603}
PB=${PORT_B:-7604}
S=${HARNESS_DIR:-$(mktemp -d /tmp/raddbg_ipc_pid.XXXXXX)}
mkdir -p $S
port_from_pid() { echo $(( 49152 + ($1 % 16384) )); }
wait_listen() { local port=$1; for i in $(seq 1 400); do lsof -nP -iTCP:$port -sTCP:LISTEN 2>/dev/null | grep -q raddbg && return 0; sleep 0.05; done; return 1; }
listens_on() { lsof -nP -iTCP:$2 -sTCP:LISTEN 2>/dev/null | grep -q " $1 "; }
fails=0

for port in $PA $PB; do pgrep -f "raddbg --ipc_port:$port" | xargs kill -9 2>/dev/null; done
rm -f $S/a.raddbg_user $S/b.raddbg_user

($RD --ipc_port:$PA --user:$S/a.raddbg_user --project:$S/a.raddbg --logs:$S/logs_a > $S/a.out 2>&1 &)
($RD --ipc_port:$PB --user:$S/b.raddbg_user --project:$S/b.raddbg --logs:$S/logs_b > $S/b.out 2>&1 &)
wait_listen $PA || { echo "A never listened on $PA"; exit 1; }
wait_listen $PB || { echo "B never listened on $PB"; exit 1; }
PIDA=$(pgrep -f "raddbg --ipc_port:$PA"); PIDB=$(pgrep -f "raddbg --ipc_port:$PB")
PPA=$(port_from_pid $PIDA); PPB=$(port_from_pid $PIDB)
echo "instance A pid=$PIDA default_port=$PA pid_port=$PPA"
echo "instance B pid=$PIDB default_port=$PB pid_port=$PPB"

# each instance must listen on its own pid-derived port
wait_listen $PPA || { echo "FAIL: A not listening on its pid port $PPA"; fails=$((fails+1)); }
wait_listen $PPB || { echo "FAIL: B not listening on its pid port $PPB"; fails=$((fails+1)); }
# routing specificity: A listens on PPA, not PPB (and vice versa), when they differ
if [[ "$PPA" != "$PPB" ]]; then
  listens_on $PIDA $PPA && ! listens_on $PIDA $PPB && echo "OK: A owns $PPA only" || { echo "FAIL: A pid-port ownership wrong"; fails=$((fails+1)); }
  listens_on $PIDB $PPB && ! listens_on $PIDB $PPA && echo "OK: B owns $PPB only" || { echo "FAIL: B pid-port ownership wrong"; fails=$((fails+1)); }
else
  echo "NOTE: pid ports collided ($PPA); skipping ownership cross-check"
fi

# drive each specific instance by --pid
ra=$($RD --ipc --pid:$PIDA state 2>&1); echo "$ra" | grep -q "version" && echo "OK: --pid:$PIDA state replied" || { echo "FAIL: --pid:$PIDA no reply"; fails=$((fails+1)); }
rb=$($RD --ipc --pid:$PIDB state 2>&1); echo "$rb" | grep -q "version" && echo "OK: --pid:$PIDB state replied" || { echo "FAIL: --pid:$PIDB no reply"; fails=$((fails+1)); }
# default-port path still works
rp=$($RD --ipc_port:$PA --ipc state 2>&1); echo "$rp" | grep -q "version" && echo "OK: --ipc_port:$PA state replied" || { echo "FAIL: default-port path broke"; fails=$((fails+1)); }

kill -9 $PIDA $PIDB 2>/dev/null
echo "----"
if [[ $fails -eq 0 ]]; then echo "PASS: --pid routes to the addressed instance; default port intact"; exit 0
else echo "FAIL: $fails checks failed"; exit 1; fi
