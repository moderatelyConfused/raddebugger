#!/bin/zsh
# Proof that the demon sweeps the symbol-only images cache (<cache>/raddbg/images/<UUID>/):
# every macOS update changes every image UUID, so without a sweep the cache only grows
# (1018 directories, 1.4 GB with the RDIs, on the author's machine). Once per debugger
# session, when the first symbol-only file is needed, the demon removes the directories
# unused for MAC_DMN_IMAGES_CACHE_MAX_AGE_DAYS (30; a reuse touches the directory) and then
# the oldest beyond MAC_DMN_IMAGES_CACHE_MAX_SIZE (1 GB). The instance runs with HOME under
# HARNESS_DIR; its images directory is seeded with two directories dated 2024 (must go) and
# one dated today (must stay), then a debuggee is stopped in main (which writes the first
# symbol-only files and sweeps). Checks: the old directories are gone, the new one and the
# real ones remain, the ctrl log records the removal.
#
# Settings: RADDBG, IPC_PORT (7611), HARNESS_DIR.
set -u
ROOT="$(cd "$(dirname ${(%):-%x})/../../.." && pwd)"
RD=${RADDBG:-"$ROOT/build/raddbg"}
P=${IPC_PORT:-7611}
S=${HARNESS_DIR:-$(mktemp -d /tmp/raddbg_sweep.XXXXXX)}
IMAGES=$S/home/Library/Caches/raddbg/images
mkdir -p $S/src $IMAGES
ipc() { $RD --ipc_port:$P --ipc "$@" 2>&1; }
field() { echo "$1" | grep -m1 " $2:" | awk '{print $2}'; }

cat > $S/src/hi.c <<'EOF'
#include <stdio.h>
int main(void) { printf("hi\n"); return 0; }
EOF
clang -g -O0 -o $S/src/hi $S/src/hi.c || { echo "hi build failed"; exit 1; }

# seed: two stale directories, one fresh
for d in 00000000-0000-0000-0000-00000000OLD1 00000000-0000-0000-0000-00000000OLD2; do
  mkdir -p $IMAGES/$d; head -c 4096 /dev/zero > $IMAGES/$d/libfake.dylib; head -c 8192 /dev/zero > $IMAGES/$d/libfake.dylib.rdi
  touch -t 202401010000 $IMAGES/$d/libfake.dylib $IMAGES/$d/libfake.dylib.rdi $IMAGES/$d
done
mkdir -p $IMAGES/00000000-0000-0000-0000-00000000NEW1; head -c 4096 /dev/zero > $IMAGES/00000000-0000-0000-0000-00000000NEW1/libfake.dylib
before=$(ls $IMAGES | wc -l | tr -d ' ')

fails=0
rm -f $S/u.raddbg_user $S/p.raddbg; rm -rf $S/logs
HOME=$S/home $RD --ipc_port:$P --user:$S/u.raddbg_user --project:$S/p.raddbg --logs:$S/logs > $S/out.txt 2>&1 &
PID=$!
for i in $(seq 1 600); do lsof -nP -iTCP:$P -sTCP:LISTEN 2>/dev/null | grep -q raddbg && break; sleep 0.05; done
ipc add_target "$S/src/hi" >/dev/null 2>&1
ipc add_function_breakpoint main >/dev/null 2>&1
ipc run >/dev/null 2>&1
for i in $(seq 1 200); do st=$(ipc state); sc=$(field "$st" stop_count); [[ "$(field "$st" running)" == "0" && -n "$sc" && "$sc" -gt 0 ]] && break; sleep 0.1; done
sym=$(ipc state | grep -m1 ip_voff_symbol | cut -d'"' -f2)
echo "stopped at main (symbol=\"$sym\")"
[[ "$sym" == main ]] || { echo "FAIL: did not stop at main"; fails=$((fails+1)); }
ipc exit >/dev/null 2>&1
for i in $(seq 1 100); do kill -0 $PID 2>/dev/null || break; sleep 0.05; done
kill -9 $PID 2>/dev/null

after=$(ls $IMAGES | wc -l | tr -d ' ')
echo "images directories: $before seeded -> $after after the stop (the debuggee's own shared-cache images were added)"
[[ ! -d $IMAGES/00000000-0000-0000-0000-00000000OLD1 && ! -d $IMAGES/00000000-0000-0000-0000-00000000OLD2 ]] || { echo "FAIL: stale directories not removed"; fails=$((fails+1)); }
[[ -d $IMAGES/00000000-0000-0000-0000-00000000NEW1 ]] || { echo "FAIL: the fresh directory was removed"; fails=$((fails+1)); }
[[ "$after" -gt 10 ]] || { echo "FAIL: the debuggee's images were not written ($after directories)"; fails=$((fails+1)); }
line=$(grep -h "mac demon: removed" $S/logs/ctrl_thread.raddbg_log 2>/dev/null | sed 's/^ *//' | head -1)
echo "${line:-no removal line in the ctrl log}"
[[ "$line" == *"removed 2 image directories"* ]] || { echo "FAIL: ctrl log does not record the removal of 2 directories"; fails=$((fails+1)); }

pkill -9 -f "^$S/src/hi" 2>/dev/null
echo "----"
if [[ $fails -eq 0 ]]; then echo "PASS: the images cache is swept (stale directories removed, fresh ones kept)"; exit 0
else echo "FAIL: $fails checks failed"; exit 1; fi
