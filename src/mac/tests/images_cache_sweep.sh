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
# Settings: RADDBG, IPC_PORT (7611), HARNESS_DIR (see lib.sh: the instance, `ipc`, `field`, the waits).
set -u
export IPC_PORT=${IPC_PORT:-7611}
source "$(cd "$(dirname ${(%):-%x})" && pwd)/lib.sh"
remove_owned_harness_dir_at_exit
# the instance's HOME (and this script's: nothing here needs the real one) is under the scratch
# directory, so the symbol-only image files and the sweep stay out of the user's cache
export HOME=$S/home
IMAGES=$S/home/Library/Caches/raddbg/images
mkdir -p $S/src $IMAGES
WAIT_STOP_POLLS=200   # the first stop includes the conversions and the sweep: 20 s

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
# a fresh project and log (dbg_start: fresh user file, an earlier instance on the port killed, the listener awaited)
rm -f $S/p.raddbg; rm -rf $S/logs
dbg_start $S/p.raddbg || { echo "FAIL: no instance listens on $IPC_PORT"; exit 1; }
PID=$RD_PID
ipc add_target "$S/src/hi" >/dev/null 2>&1
ipc add_function_breakpoint main >/dev/null 2>&1
st=$(run_stop)
sym=$(stop_symbol "$st")
echo "stopped at main (symbol=\"$sym\")"
[[ "$sym" == main ]] || { echo "FAIL: did not stop at main"; fails=$((fails+1)); }
dbg_stop

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
