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
# Also: `state` reports the instance's pid (`pid:`), and `--ipc --pid:<dbgpid> attach --pid:<target>`
# attaches the addressed instance to the target (the routing `--pid` before the command name
# is the client's; it used to be forwarded as command text, and `attach` then took the
# debugger's own pid, since cmd_line_insert_opt keeps the first occurrence of an option).
#
# Settings: RADDBG, PORT_A (7603), PORT_B (7604), HARNESS_DIR, SLEEPER (a long-lived target
# binary; built if missing).
set -u
RD=${RADDBG:-"$(cd "$(dirname ${(%):-%x})/../../.." && pwd)/build/raddbg"}
PA=${PORT_A:-7603}
PB=${PORT_B:-7604}
S=${HARNESS_DIR:-$(mktemp -d /tmp/raddbg_ipc_pid.XXXXXX)}
mkdir -p $S
SLEEPER=${SLEEPER:-"$S/sleeper"}
# rd_ipc_port_from_pid (raddbg_main.c): a window of the registered range, 20000..39999
port_from_pid() { echo $(( 20000 + ($1 % 20000) )); }
field() { echo "$1" | grep -m1 " $2:" | awk '{print $2}'; }
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

# drive each specific instance by --pid; `state` names the instance which answered
ra=$($RD --ipc --pid:$PIDA state 2>&1); echo "$ra" | grep -q "version" && echo "OK: --pid:$PIDA state replied" || { echo "FAIL: --pid:$PIDA no reply"; fails=$((fails+1)); }
rb=$($RD --ipc --pid:$PIDB state 2>&1); echo "$rb" | grep -q "version" && echo "OK: --pid:$PIDB state replied" || { echo "FAIL: --pid:$PIDB no reply"; fails=$((fails+1)); }
[[ "$(field "$ra" pid)" == "$PIDA" ]] && echo "OK: --pid:$PIDA reached the instance with pid $PIDA" || { echo "FAIL: --pid:$PIDA reached pid '$(field "$ra" pid)'"; fails=$((fails+1)); }
[[ "$(field "$rb" pid)" == "$PIDB" ]] && echo "OK: --pid:$PIDB reached the instance with pid $PIDB" || { echo "FAIL: --pid:$PIDB reached pid '$(field "$rb" pid)'"; fails=$((fails+1)); }
# default-port path still works
rp=$($RD --ipc_port:$PA --ipc state 2>&1); echo "$rp" | grep -q "version" && echo "OK: --ipc_port:$PA state replied" || { echo "FAIL: default-port path broke"; fails=$((fails+1)); }

# ipc_pid_attach: attach by pid through --pid routing; the command's own --pid is the target's
if [[ ! -x "$SLEEPER" ]]; then
  cat > $S/sleeper.c <<'EOF'
#include <stdio.h>
#include <unistd.h>
int main(void){ printf("sleeper pid %d\n", getpid()); fflush(stdout); for(int i=0;i<120;i++) sleep(1); return 0; }
EOF
  clang -g -O0 -o $S/sleeper $S/sleeper.c || { echo "could not build sleeper"; exit 1; }
  SLEEPER="$S/sleeper"
fi
$SLEEPER >/dev/null 2>&1 &
TPID=$!
sleep 0.2
$RD --ipc --pid:$PIDA "attach --pid:$TPID" >/dev/null 2>&1
attached=""
for i in $(seq 1 50); do
  sa=$($RD --ipc --pid:$PIDA state 2>&1)
  attached=$(echo "$sa" | sed -n '/^ processes:/,/^ call_stack:/p' | grep "   id:" | awk '{print $2}' | head -1)
  [[ "$attached" == "$TPID" ]] && break
  sleep 0.1
done
[[ "$attached" == "$TPID" ]] && echo "OK: ipc_pid_attach: --pid:$PIDA attach --pid:$TPID attached the sleeper ($TPID)" || { echo "FAIL: ipc_pid_attach: instance A lists process '$attached', expected the sleeper $TPID"; fails=$((fails+1)); }
sb=$($RD --ipc --pid:$PIDB state 2>&1)
[[ "$(echo "$sb" | sed -n '/^ processes:/,/^ call_stack:/p' | grep -c '   id:')" == "0" ]] && echo "OK: instance B has no process (the attach reached A only)" || { echo "FAIL: instance B got a process"; fails=$((fails+1)); }
$RD --ipc --pid:$PIDA "kill --process:$TPID" >/dev/null 2>&1
sleep 0.3
kill -9 $TPID 2>/dev/null

kill -9 $PIDA $PIDB 2>/dev/null
echo "----"
if [[ $fails -eq 0 ]]; then echo "PASS: --pid routes to the addressed instance; default port intact"; exit 0
else echo "FAIL: $fails checks failed"; exit 1; fi
