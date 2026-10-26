#!/bin/zsh
# Proof that the IPC `exit` command quits an instance that has a LIVE debuggee.
#
# Previously `raddbg --ipc exit` was ignored while attached to a live process: the
# GUI Exit command opens an "are you sure?" confirmation popup that an IPC client
# cannot answer, so the instance stayed alive (README open issue: "exit over IPC
# does not quit an instance with a live process (kill it by pid)"). An external
# driver's command is never answered a confirmation for: a plain `exit` with a live
# debuggee is refused with an error in its reply (the instance stays, no popup
# opens), and `exit --force_confirm:1` exits and tears down the debuggee exactly
# like the GUI's confirmed close.
#
# The script launches an instance, adds and runs a long-lived target (so there is
# a live debuggee), confirms running==1 and the debuggee process exists, sends a
# plain `exit` (must be refused, instance still alive), then `exit
# --force_confirm:1`, and checks the instance process is gone shortly after (and
# reports what happened to the debuggee).
#
# Settings: RADDBG, SLEEPER (a long-lived target binary; built if missing),
# IPC_PORT (7607), HARNESS_DIR.
set -u
ROOT="$(cd "$(dirname ${(%):-%x})/../../.." && pwd)"
RD=${RADDBG:-"$ROOT/build/raddbg"}
P=${IPC_PORT:-7607}
# a scratch directory of our own is removed at exit; a HARNESS_DIR given by the caller is theirs
if [[ -n ${HARNESS_DIR:-} ]]; then S=$HARNESS_DIR; else S=$(mktemp -d /tmp/raddbg_ipc_exit.XXXXXX); trap 'rm -rf "$S"' EXIT; fi
SLEEPER=${SLEEPER:-"$S/sleeper"}
mkdir -p $S
ipc() { $RD --ipc_port:$P --ipc "$@" 2>&1; }
field() { echo "$1" | grep -m1 " $2:" | awk '{print $2}'; }
# the instance is found by its listener (lsof, LISTEN on the port) and killed by that pid
listener_pid() { lsof -nP -iTCP:$P -sTCP:LISTEN 2>/dev/null | awk 'NR>1 && $1 ~ /raddbg/ {print $2; exit}'; }
kill_port() { local pid=$(listener_pid); [[ -n "$pid" ]] && kill -9 $pid 2>/dev/null; }

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

kill_port
rm -f $S/u.raddbg_user
($RD --ipc_port:$P --user:$S/u.raddbg_user --project:$S/p.raddbg --logs:$S/logs > $S/out.txt 2>&1 &)
for i in $(seq 1 400); do lsof -nP -iTCP:$P -sTCP:LISTEN 2>/dev/null | grep -q raddbg && break; sleep 0.05; done
(ipc state >/dev/null 2>&1 &)
for i in $(seq 1 100); do ipc state | grep -q version && break; sleep 0.1; done
PID=$(listener_pid)
echo "instance pid $PID"
[[ -n "$PID" ]] || { echo "FAIL: no instance listens on $P"; exit 1; }

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

echo "=== sending a plain IPC exit (must be refused: a live debuggee needs --force_confirm:1) ==="
refusal=$(ipc exit 2>&1 | head -1)
echo "reply: $refusal"
sleep 0.5
refused=0
if echo "$refusal" | grep -q "^error: exit: still attached" && kill -0 $PID 2>/dev/null; then refused=1; fi
echo "plain exit refused, instance still alive: $refused"
if [[ $refused -ne 1 ]]; then echo "FAIL: a plain exit with a live debuggee was not refused"; kill -9 $PID 2>/dev/null; kill -9 $DBG_PID 2>/dev/null; exit 1; fi

echo "=== sending IPC exit --force_confirm:1 ==="
ipc exit --force_confirm:1 >/dev/null 2>&1
# the instance must terminate shortly, and take its debuggee down with it (the GUI's confirmed
# close kills the attached processes; a debuggee left running would be an orphan of the test)
gone=0
for i in $(seq 1 50); do kill -0 $PID 2>/dev/null || { gone=1; break; }; sleep 0.1; done
dbg_gone=0
for i in $(seq 1 30); do kill -0 $DBG_PID 2>/dev/null || { dbg_gone=1; break; }; sleep 0.1; done

echo "instance terminated after exit: $gone"
echo "debuggee (pid $DBG_PID) terminated: $dbg_gone"

# cleanup
kill -9 $PID 2>/dev/null
[[ $dbg_gone -eq 0 ]] && kill -9 $DBG_PID 2>/dev/null
echo "----"
if [[ $gone -eq 1 && $dbg_gone -eq 1 ]]; then
  echo "PASS: IPC exit quit the instance despite a live debuggee, and the debuggee with it"
  exit 0
else
  [[ $gone -eq 1 ]]     || echo "FAIL: instance still running after IPC exit"
  [[ $dbg_gone -eq 1 ]] || echo "FAIL: the debuggee (pid $DBG_PID) survived the instance's exit"
  exit 1
fi
