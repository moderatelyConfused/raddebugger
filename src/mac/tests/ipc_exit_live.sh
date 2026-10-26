#!/bin/zsh
# Proof that the IPC `exit` command quits an instance that has a LIVE debuggee.
#
# Previously `raddbg --ipc exit` was ignored while attached to a live process: the
# GUI Exit command opens an "are you sure?" confirmation popup that an IPC client
# cannot answer, so the instance stayed alive (README open issue: "exit over IPC
# does not quit an instance with a live process (kill it by pid)"). The external
# driver (IPC) now force-confirms Exit, so it exits and tears down the debuggee
# exactly like the GUI's confirmed close.
#
# The script launches an instance, adds and runs a long-lived target (so there is
# a live debuggee), confirms running==1 and the debuggee process exists, sends
# `exit`, and checks the instance process is gone shortly after (and reports what
# happened to the debuggee).
#
# Settings: RADDBG, SLEEPER (a long-lived target binary; built if missing),
# IPC_PORT (7607), HARNESS_DIR.
set -u
ROOT="$(cd "$(dirname ${(%):-%x})/../../.." && pwd)"
RD=${RADDBG:-"$ROOT/build/raddbg"}
P=${IPC_PORT:-7607}
S=${HARNESS_DIR:-$(mktemp -d /tmp/raddbg_ipc_exit.XXXXXX)}
SLEEPER=${SLEEPER:-"$S/sleeper"}
mkdir -p $S
ipc() { $RD --ipc_port:$P --ipc "$@" 2>&1; }
field() { echo "$1" | grep -m1 " $2:" | awk '{print $2}'; }

# build a long-lived debuggee if one was not provided
if [[ ! -x "$SLEEPER" ]]; then
  cat > $S/sleeper.c <<'EOF'
#include <stdio.h>
#include <unistd.h>
int main(void){ printf("sleeper pid %d\n", getpid()); fflush(stdout); for(int i=0;i<120;i++) sleep(1); return 0; }
EOF
  clang -g -O0 -o $S/sleeper $S/sleeper.c || { echo "could not build sleeper"; exit 1; }
  SLEEPER="$S/sleeper"
fi

pgrep -f "raddbg --ipc_port:$P" | xargs kill -9 2>/dev/null
rm -f $S/u.raddbg_user
($RD --ipc_port:$P --user:$S/u.raddbg_user --project:$S/p.raddbg --logs:$S/logs > $S/out.txt 2>&1 &)
for i in $(seq 1 400); do lsof -nP -iTCP:$P -sTCP:LISTEN 2>/dev/null | grep -q raddbg && break; sleep 0.05; done
(ipc state >/dev/null 2>&1 &)
for i in $(seq 1 100); do ipc state | grep -q version && break; sleep 0.1; done
PID=$(pgrep -f "raddbg --ipc_port:$P")
echo "instance pid $PID"

ipc add_target "$SLEEPER" >/dev/null 2>&1
ipc run >/dev/null 2>&1
# wait for the debuggee to be live
live=0
for i in $(seq 1 60); do
  st=$(ipc state)
  if [[ "$(field "$st" running)" == "1" ]] && pgrep -f "$SLEEPER" >/dev/null; then live=1; break; fi
  sleep 0.1
done
DBG_PID=$(pgrep -f "$SLEEPER" | head -1)
echo "running=$(field "$(ipc state)" running)  debuggee pid=$DBG_PID  (live precondition: $live)"
if [[ $live -ne 1 ]]; then echo "FAIL: could not establish a live debuggee"; kill -9 $PID 2>/dev/null; exit 1; fi

echo "=== sending IPC exit ==="
ipc exit >/dev/null 2>&1
# the instance must terminate shortly
gone=0
for i in $(seq 1 50); do pgrep -f "raddbg --ipc_port:$P" >/dev/null || { gone=1; break; }; sleep 0.1; done
sleep 0.3
dbg_gone=1; kill -0 $DBG_PID 2>/dev/null && dbg_gone=0

echo "instance terminated after exit: $gone"
echo "debuggee (pid $DBG_PID) terminated: $dbg_gone"

# cleanup
pgrep -f "raddbg --ipc_port:$P" | xargs kill -9 2>/dev/null
[[ $dbg_gone -eq 0 ]] && kill -9 $DBG_PID 2>/dev/null
echo "----"
if [[ $gone -eq 1 ]]; then
  echo "PASS: IPC exit quit the instance despite a live debuggee (debuggee terminated: $dbg_gone)"
  exit 0
else
  echo "FAIL: instance still running after IPC exit"
  exit 1
fi
