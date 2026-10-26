#!/bin/zsh
# Stress: 32 threads hit one breakpoint over and over; 200 continues. Every continue must stop at the
# breakpoint (never run away, never die), and no stop may be answered without having been reported:
# the demon logs "has an unreported pending exception at a resume" if a stop which arrived outside a
# round (during an internal step) were about to be continued silently, and "a stop received outside a
# round is pending" when it reports such a stop instead of resuming (counted for information).
source "$(dirname ${(%):-%x})/common.sh"
LDFLAGS_EXTRA="-lpthread" build_c bp_loop
write_project bp_loop $S/bp_loop ""
launch $S/bp_loop.raddbg
bp $DEMON_TESTS_DIR/src/bp_loop.c:$(line_of bp_loop "// bp_work")
rounds=${BP_STRESS_ROUNDS:-200}
stops=0 misses=0
for i in $(seq 1 $rounds); do
  st0=$(ipc state); before=$(field "$st0" stop_count); before=${before:-0}
  ipc run >/dev/null
  st=$(wait_stop $before x)
  if [[ "$(field "$st" stop_count)" -gt "$before" && $(stop_symbol "$st") == work ]]; then stops=$((stops+1)); else misses=$((misses+1)); fi
  [[ $(log_events ExitProcess) != 0 ]] && break
done
st=$(ipc state)
echo "  INFO  $stops stop(s) in work over $rounds continue(s), $misses other; $(grep -c 'a stop received outside a round is pending' $CTRL_LOG 2>/dev/null) stop(s) reported from outside a round; $(grep -c 'held_back_breakpoint_hit: .* reported now' $CTRL_LOG 2>/dev/null) held-back hit(s) reported; threads at the end: $(thread_count "$st")"
check "every continue stopped at the breakpoint in work ($stops of $rounds)" '[[ $stops == $rounds ]]'
check "the process is alive with all 33 threads" '[[ $(log_events ExitProcess) == 0 && $(thread_count "$st") == 33 ]]'
check "no pending exception was answered unreported" '! grep -q "has an unreported pending exception at a resume" $CTRL_LOG'
finish bp_stress
