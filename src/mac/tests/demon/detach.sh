#!/bin/zsh
# Detach: the debugger forgets the process within a second and the debuggee runs on; with two attached
# processes, detaching one leaves the other known and runnable.
# Needs the Zig generator project (GENERATOR_SRC, default ~/Desktop/Etc/October-2026/generator) and zig
# (ZIG, default ~/.zvm/bin/zig); the generator is built (debug, arm64) into the harness directory.
source "$(dirname ${(%):-%x})/common.sh"
GEN_SRC=${GENERATOR_SRC:-$HOME/Desktop/Etc/October-2026/generator}
ZIG=${ZIG:-$HOME/.zvm/bin/zig}
(cd $GEN_SRC && $ZIG build -p $S/generator --cache-dir $S/zig-cache) || { echo "generator build failed"; exit 1; }
GEN=$S/generator/bin/generator
$GEN > $S/generator.expected.txt
GEN_BP=$GEN_SRC/src/main.zig:$(grep -n -m1 "while (try rng.next())" $GEN_SRC/src/main.zig | cut -d: -f1)
write_project generator $GEN "" </dev/null
build_c sleeper

echo "== detach: one process"
launch $S/generator.raddbg
bp $GEN_BP
go
t0=$(date +%s)
ipc detach >/dev/null
wait_no_threads 10; r=$?
check "no process reported within a second of the detach" "[[ $r == 0 ]]"
wait_process_gone $GEN 50; r=$?
check "the detached generator ran to completion" "[[ $r == 0 ]]"
check "its output is complete" "cmp -s $S/generator.stdout.txt $S/generator.expected.txt"
check "one ExitProcess event, no Exception event" "[[ $(log_events ExitProcess) == 1 && $(log_events Exception) == 0 ]]"

echo "== detach: two processes (the sleeper is attached by pid, the generator detached at its breakpoint)"
rm -f $S/sleeper_out.txt $S/generator.stdout.txt
launch $S/generator.raddbg
bp $GEN_BP
$S/sleeper $S/sleeper_out.txt &
SLEEPER_PID=$!
sleep 0.3
before=$(field "$(ipc state)" stop_count)
ipc "attach --pid:$SLEEPER_PID" >/dev/null
st=$(wait_stop ${before:-0} x)
sleep 0.3
st=$(ipc state)
check "attached to the sleeper: two processes' threads known ($(thread_count "$st"))" "[[ $(thread_count "$st") -ge 2 ]]"
go
st=$(ipc state)
check "stopped at the generator's breakpoint" "echo '$st' | grep -q 'main.zig'"
ipc detach >/dev/null
sleep 0.5
st=$(ipc state)
check "only the sleeper's thread remains known" "[[ $(thread_count "$st") == 1 ]]"
wait_process_gone $GEN 50; r=$?
check "the detached generator ran to completion" "[[ $r == 0 ]]"
check "its output is complete" "cmp -s $S/generator.stdout.txt $S/generator.expected.txt"
c0=$(cat $S/sleeper_out.txt)
ipc run >/dev/null
sleep 0.6
c1=$(cat $S/sleeper_out.txt)
check "the sleeper runs after the other process was detached (counter $c0 -> $c1)" "[[ $c1 -gt $c0 ]]"
ipc kill_all >/dev/null
finish detach
