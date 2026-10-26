# macOS (Apple Silicon) Port Notes

Developer notes for the macOS/arm64 port. User-facing build instructions are
in the top-level README ("macOS (Apple Silicon)").

## Layout

| Layer | Files | Notes |
|-------|-------|-------|
| base | `mac/base/mac_base.{h,c}` | POSIX/Mach port of `linux/base`. Named semaphores & shared memory names are hashed to fit macOS' 31-char limit. Unnamed semaphores are Mach semaphores. |
| socket | `mac/socket/mac_socket.{h,c}` | kqueue instead of epoll, `SO_NOSIGPIPE` instead of `MSG_NOSIGNAL`. |
| demon | `mac/demon/mac_demon.{h,c}` | Process control (see below). |
| window manager | `mac/window_manager/mac_window_manager.{h,c}` | Thin C layer over the platform API. |
| shell | `mac/shell/mac_shell.{h,c}` | Alerts / file panels / `open` via the platform API. |
| platform | `mac/mac_platform.{h,m}` | The ONLY file that includes Apple framework headers (Cocoa, OpenGL, CoreText). Compiled separately as Objective-C by `build.sh` and linked into graphical targets. Exposes a plain C API (`mac_plat_*`). |
| render | `render/opengl/mac/render_opengl_mac.{h,c}` | NSOpenGLContext (GL 4.1 core) through the platform API; GL entry points resolved with `dlsym`. |
| fonts | `font_provider/coretext/font_provider_coretext.{h,c}` | CoreText rasterization through the platform API. |
| arch | `arm64/arm64.{h,c,mdesk}`, `arm64/disasm/` | Register block, DWARF/RDI register maps, AArch64 disassembler. |
| binaries | `macho/macho*.{h,c}`, `dwarf/dwarf_parse_macho.{h,c}`, `rdi_from_macho/` | Mach-O parsing, DWARF extraction from `__DWARF` (dSYM), RDI conversion. |
| unwinding | `macho/unwind/macho_unwind.{h,c}` | Apple compact unwind (`__unwind_info`), falling back to `__eh_frame` FDEs and finally the frame-pointer chain. |

The unity build on macOS includes only libc / POSIX / Mach headers, so it can
be syntax-checked on any host with Apple's open-source headers (e.g. the ones
bundled with Zig: `zig cc -target aarch64-macos -fsyntax-only ...`).

## Demon (process control)

* Launch: `posix_spawn` with `POSIX_SPAWN_START_SUSPENDED`, `task_for_pid`,
  `task_set_exception_ports` (EXCEPTION_DEFAULT | MACH_EXCEPTION_CODES) and
  `ptrace(PT_ATTACHEXC)` so BSD signals are delivered as `EXC_SOFTWARE` /
  `EXC_SOFT_SIGNAL` exceptions. The first `dmn_ctrl_run` reports process,
  thread and module creation while the process is still stopped at dyld's
  first instruction; the main executable's header address is taken from the
  top of the stack and dyld's from a page scan below the PC.
* Attach: `task_for_pid` + `PT_ATTACHEXC`; the resulting `SIGSTOP` soft
  signal is the initial stop. Launch and attach fail when the exception ports
  cannot be installed.
* Detach: `PT_DETACH` needs the BSD process in `SSTOP`, which under
  `PT_ATTACHEXC` only holds while a thread is inside a soft-signal exception,
  and the signal of that exception is delivered once the tracer is gone. So
  `dmn_ctrl_detach` answers the pending exceptions (hardware ones escalated to
  their signal), lets the process run, sends it `SIGCONT` (a traced process
  reports every signal), restores the exception ports and `PT_DETACH`es at
  that exception, then replies with plain continues and resumes; the `SIGCONT`
  the process takes afterwards does nothing. The process is marked detached and
  the next `dmn_ctrl_run` synthesizes `ExitThread` / `UnloadModule` /
  `ExitProcess` and frees it (Win32's queued-detach path). If `PT_DETACH`
  fails the process stays attached and the detach reports failure.
* Waiting: a kqueue with `EVFILT_MACHPORT` (exception port set),
  `EVFILT_PROC` (`NOTE_EXIT`, `NOTE_EXEC`; `NOTE_EXITSTATUS` only for our own
  children, the kernel refuses it otherwise) and `EVFILT_USER` (halt requests).
* Stops: on the first exception message the whole task is suspended with
  `task_suspend`, stragglers are drained, `task_threads` is diffed against the
  known thread list (thread create/exit events), and registers are read with
  `thread_get_state` (`ARM_THREAD_STATE64`, `ARM_NEON_STATE64`).
* Resume: pending exception messages are replied to (`PT_THUPDATE` chooses
  the signal to deliver for soft signals; `KERN_FAILURE` escalates a hardware
  exception to its BSD signal when the engine did not ask to ignore it),
  frozen threads are `thread_suspend`ed, then `task_resume`. A frozen thread
  keeps its pending exception (and its reported mark) until it runs again: the
  run which resumes it decides what to do with it, as on Linux. A soft signal
  which is the kernel's escalation of a hardware exception the engine passed
  (same pc, the exception's signal) is not re-reported as a repeated exception;
  it is delivered whatever the next run's ignore flag says, so signal handlers
  run.
* Breakpoints: `brk #0`; the PC points at the trap instruction when reported
  (no rollback). Single stepping: `MDSCR_EL1.SS` via `ARM_DEBUG_STATE64`.
  Watchpoints: `DBGWCR/DBGWVR` via `ARM_DEBUG_STATE64`; since arm64 reports
  them before the access, the demon steps past the access internally; the
  event carries the trap id (`user_data`), which attributes the stop to its
  data breakpoint.
* Forked children inherit the exception ports and the trap instructions in
  their copy-on-write image. An exception from a task the demon does not debug
  is failed (`KERN_FAILURE`: the child takes the signal's default action, or
  its handler), unless it is an `EXC_BREAKPOINT` at one of our traps (the dyld
  trap, or a trap of the run in progress, found through the child's
  ancestry), in which case the original instruction is written back into the
  child and it continues. Children are not debugged (`debug_subprocesses` is
  recorded, not acted on).
* Exec: the kernel replaces the task's kernel port but keeps the exception
  ports, so the post-exec `SIGTRAP` soft signal arrives under a new task port
  name; it is matched to the process by pid and the new port adopted. A
  `SIGTRAP` at a fresh image (`dyld_all_image_infos` with an empty image list)
  unloads every module, reports the vanished threads, re-runs the launch-style
  discovery (main header at `[sp]`, dyld by scan from `pc`) and is swallowed;
  traps written before the exec are not restored into the new image. Exec of
  a platform binary (e.g. `/bin/echo`) kills the process with `SIGTRAP`: the
  exec drops the exception ports for a debugger without Apple's private
  task-port entitlement (lldb dies the same way).
* Modules: a permanent `brk` on dyld's debugger-notification function
  (`x0` = mode, `x1` = count, `x2` = `dyld_image_info[]`) reports loads and
  unloads; the demon steps over its own trap internally. Mode 3
  (`dyld_image_dyld_moved`, dyld handing over to its copy in the shared cache)
  re-targets the trap to the new dyld. The notifier address is the pointer dyld
  publishes in `dyld_all_image_infos.notification` (on this macOS the
  shared-cache dyld's `_lldb_image_notifier`; its local symbol is not in the
  on-disk dyld's symbol table, so the published pointer, PAC-stripped, is the
  reliable source, with a symbol lookup — `_dyld_debugger_notification`,
  `__dyld_debugger_notification`, `_lldb_image_notifier` — as a fallback). As a
  safety net, every stop re-reads `dyld_all_image_infos` and diffs the image
  list against the last seen count. The struct address captured at launch
  belongs to the initial dyld and goes stale once dyld hands over (the old
  struct keeps reporting one image), so the demon reads both the cached address
  and the one `TASK_DYLD_INFO` currently reports and keeps whichever lists more
  images (`mac_dmn_process_read_all_image_infos`); the trap is (re)armed from
  that struct's notifier. Module info is parsed from the in-memory Mach-O header
  (`__unwind_info`, `__eh_frame`, UUID, entry point, `__thread_vars`).
* Symbol tables of shared-cache images: the dyld shared cache's images have no
  file of their own on this macOS (`/usr/lib/libSystem.B.dylib` is not on
  disk; `/usr/lib/system/libsystem_kernel.dylib` is, but as a fat file whose
  slices are not the cache's copy) and no DWARF. Their symbol tables are
  reachable through the task port: `LC_SYMTAB`'s cache-relative offsets map
  into memory through the `__LINKEDIT` segment, as the demon already does to
  find dyld's notifier. For an image whose path is not a file with its UUID
  (`MH_DYLIB_IN_CACHE` in the header skips that probe),
  `mac_dmn_debug_info_path_from_image` writes a symbol-only Mach-O (header and
  load commands from memory; the defined symbols, their names and
  `LC_FUNCTION_STARTS` appended; segments without file data, like a dSYM) to
  `<cache>/raddbg/images/<UUID>/<name>`, once per UUID, and reports it as the
  module's debug info path. The engine converts it like any Mach-O
  (`radbin --rdi`): without `__DWARF`, `rdi_from_macho.c` turns `LC_SYMTAB`'s
  defined code symbols into procedures (`_write` -> `write`, link name kept),
  each ending at the next symbol, the next function start or its section's
  end. The cache files under
  `/System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld/` hold the same
  data, but using them means parsing the cache header, image and mapping
  tables across the sub-cache files of exactly the cache the debuggee maps, a
  layout that changes between releases; memory reuses the parsing the demon
  does for every image anyway. Extracting dylibs with
  `dyld_shared_cache_extract_dylibs` was ruled out as a runtime dependency.
* Local symbols of shared-cache images: the cache this macOS (26.6) ships is a
  development-type cache (`dyld_cache_header.cacheType` 0; its header has
  `localSymbolsOffset` 0 and a zero `symbolFileUUID`, and no `.symbols` file
  exists on disk), and every image's `LC_SYMTAB` keeps its locals:
  libsystem_c's 2268 symbols are 1299 `N_EXT`, 200 `N_PEXT` and 769 plain
  locals, exactly the set lldb lists for the image (its 284 extra rows are
  synthesized stub names), and `__sflush`, `__sfvwrite` are even exported. So
  `__sflush`, `__vfprintf`, ... name frames and take breakpoints from the
  symbol-only file alone; `tests/dyld_module_events.sh` breaks on `__vfprintf`,
  which is private to the image. (The earlier note that the locals were missing
  mistook the 2268 for exports.)
* Cost and cache of the symbol-only files: a C program's 45 startup images
  take 2 MB of symbol-only files plus 6 MB of RDIs the engine writes next to
  them (8 MB, 180 KB per image; the author's mixed cache averages 430 KB per
  image, frameworks like WebCore reach 70 MB), written during the first stop of
  the first run; later runs reuse them. Every macOS update changes every UUID,
  so the cache only grew (1018 directories, 1.4 GB, on the author's machine):
  once per debugger session, when the first symbol-only file is needed, the
  demon removes `images/<UUID>` directories unused for 30 days (a reuse touches
  the directory) and then the oldest beyond 1 GB
  (`mac_dmn_delete_stale_subdirectories`; one listing of `images/` and of each
  directory, a few ms; `tests/images_cache_sweep.sh`). The `dsyms/` cache
  radbin fills (187 bundles, 126 MB here) and `rdis/` are not swept.
* Exception codes reported to the engine: BSD signal numbers, or
  `0x10000 + EXC_*` for Mach exception types (see `mac/mac.mdesk`); soft
  `SIGSEGV` / `SIGBUS` and `EXC_BAD_ACCESS` report the fault address.
* TLS: `dmn_thread_get_module_tls_vaddr` returns the thread's TLS block for
  an image (`*(TPIDRRO_EL0 + 8*key)`, key from the image's first TLV
  descriptor).

## Unwinding

* `macho_uwnd_step` looks the pc up in `__unwind_info`: DWARF-mode entries go
  through their `__eh_frame` FDE, frame / frameless entries are decoded
  directly, and anything else falls back to the frame-pointer chain.
* None of those rules hold while a prologue is still building the frame, or
  after an epilogue has restored the frame record (compact entries describe
  the frame only once it is set up, and LLVM emits little or no epilogue CFI),
  yet stepping stops at exactly such places: the first instruction after
  stepping into a call, or a `ret`. `uwnd_arm64_code_state_from_pc`
  (`unwind/unwind.c`) replays the prologue instructions from the function
  start (or, without a known start, recognizes a frame setup ahead of pc) and
  scans ahead for a return; in those states the return address is `lr`, the
  caller's `fp` is still in `fp`, and only the entry stack pointer is computed
  from the code. In DWARF mode the CFI row is evaluated with `fp` pointed back
  at the intact frame record, so that the callee-saved registers are restored
  from their slots as usual.
* The analysis reads the code through the unwind's `MemoryMap`, which the
  engine fills on demand: when the window around pc (`[pc-128, pc+128)`,
  clamped to pc's page, always including a known function start) is not in
  the map yet, the analysis reports `UWND_ARM64_CodeState_NeedRead` and the
  unwinders return `UWND_StepStatus_FailedMemoryRead` with that range; the
  engine reads it and retries the step (one extra round trip for the stopped
  frame). Before this the reads silently failed and every pc looked like a
  function body, so the frame-pointer rule was applied at a `ret` or in a
  prologue and the caller's frame went missing (step out then overshot by a
  frame).
* The forward scan treats every instruction which can be shown not to write
  `sp`, `x29` or `x30` as transparent (`uwnd_arm64_code_is_frame_neutral`),
  lets conditional branches fall through, and stops at calls, traps, frame
  record restores, pushes and unknown instructions. Reaching `ret` that way
  means `lr` is intact and the entry `sp` is `sp` plus the pops passed, which
  also covers frameless leaves that never store a frame record (dyld's
  `_tlv_get_addr`, which every Zig `threadlocal` access calls): a step into
  such a line now shows the caller as frame 1, and step out returns to it. A
  `b`/`br` right after the frame record pop (or any stack pop) is a tail call.
  The prologue replay is relaxed the same way, so argument moves between the
  stack adjustment and the stores do not end it.
* Outer frames hold return addresses. `UWND_ModuleInfo.pc_is_return_address`
  (set by the engine for every frame but the first) makes the module lookup,
  the `__unwind_info` lookup, the FDE search and the CFI row selection use
  `pc-1`, the call's own address, and skips the code analysis (the code at a
  return address has not run). Without it a function ending in a call to a
  noreturn function was attributed to the function that follows it in the
  binary, and the unwind stopped there. A CIE with the `S` augmentation marks
  a signal trampoline: `UWND_StepResult.is_signal_frame` is surfaced so the
  engine does not treat the frame above it as a return address.
* The end of a frame-pointer chain (`fp == 0`) is the outermost frame (pc 0,
  a good step) rather than an error, in both the compact-frame rule and the
  moduleless unwinder, and the engine keeps the frames it has when an outer
  frame's step fails or needs memory it cannot read; a thread blocked in
  `sleep` shows `__semwait_signal` up to `thread_start`. When the code at the
  *stopped* pc cannot be read at all (a call through a bad pointer),
  `pc_code_unreadable` is set and the moduleless unwinder treats the stop as
  the callee's first instruction (return address in `lr`), as lldb does.
* Function starts. `UWND_ModuleInfo.function_vaddr` carries the start of the
  function containing the stopped frame's pc, which `d_unwind_from_thread`
  takes from the RDI procedure containing it (`rdi_scope_from_voff` ->
  `rdi_procedure_from_scope` -> root scope's first voff; nil while the debug
  info is still converting). Both unwinders prefer it to the compact entry's
  offset, because the linker shares one `__unwind_info` entry among
  consecutive functions with the same encoding (at -O0 every function is
  `0x04000000`, so `basic_O0` has one entry for `0x7a4..0xb5c`, and
  `locals_frame` at `0x8a0` "started" 252 bytes earlier): the replay from the
  entry's offset either did not run (too far) or ran off at the previous
  function's `ret`, and a stop at a function's first instruction (an
  instruction step into a call) showed `locals_frame start`. With the start
  known the replay has nothing to replay at the entry and reports the prologue
  state (lldb's rule: pc = lr, cfa = sp); the frame-setup-ahead heuristic now
  also runs whenever the replay did not establish the prologue, so an unknown,
  far or wrong start degrades to the old behaviour instead of to the frame
  pointer chain. Functions the unwind info does not describe get the same
  treatment through the moduleless fallback: libsystem_kernel's syscall stubs
  (`write`: `mov x16, #N; svc; b.lo; b cerror; ret`, no compact entry; the
  `b cerror` ahead of the `ret` hides the frameless leaf from the epilogue
  scan, so the start from the symbol-table RDI is what makes `main` appear;
  `tests/dyld_module_events.sh` checks it). Linker stubs have neither a symbol
  nor an entry, so `uwnd_arm64_stub_inst_count_from_code` recognizes their
  code (`adrp x16; ldr x16, [x16, #off]; br x16`, and the arm64e
  `__auth_stubs` form) at any of its instructions: a step into a call through
  `__stubs` (`strlen`) now shows the caller as frame 1, and a step out from the
  stub or from a function's entry returns to it (flows
  `stepping_O0/inst_into_call_stack`, `into_strlen_frame1_is_caller`,
  `inst_out`). Not covered: a known start more than 128 bytes before pc with
  the frame still unbuilt (the window is 32 instructions behind pc), and the
  error path of a syscall stub past its `b.lo` (a conditional branch ends the
  replay).
* Decoder: `stp`/`ldp` to `[sp]` are recognized by `(code & 0x3a000000) ==
  0x28000000` (bit 25 clear); the old mask also matched logical
  shifted-register ops with `Rn = xzr`, so `mov xN, xM` counted as a 16-byte
  pair store. `src/mac/tests/unwind_arm64_test.c` checks the decoder and the
  analysis on hand-assembled sequences (build line in its header; 110 checks,
  among them the syscall stub, the linker stubs and the shared entry above);
  `./run_tests.sh` builds and runs it on macOS/arm64 before torture.

## Display scaling

`wm_dpi_from_window` / `wm_dpi_from_monitor` report 96 × the backing scale
factor (192 on Retina displays), and all window geometry, mouse positions and
rendering use backing pixels. The frontend treats the configured `font_size`
settings as 96-DPI values and multiplies them by DPI/96 (`rd_dpi_scale` in
`raddbg_core.c`), so configs are portable between displays, and a window
dragged to a screen with a different scale factor only re-rasterizes its fonts
(`fnt_reset`); the settings themselves are never rewritten.

## Working directory

`dbg_info` spawns the converter (`raddbg --bin --rdi ...`) with the
debugger's initial working directory (upstream used the executable's
directory), and the `dsymutil` fallback in `radbin.c` inherits it, so relative
`N_OSO` entries (from `ld -oso_prefix`) resolve from wherever the user
launched the debugger. The file browser behind Add Target / Open File is
unchanged from upstream: it starts at the user file's `current_path`, the
directory last browsed in a file query, and only falls back to the working
directory when nothing has been stored yet.

## Signing

`task_for_pid` requires the `com.apple.security.cs.debugger` entitlement
(`mac/raddbg.entitlements`); `build.sh` applies it with an ad-hoc signature.

## Status and open issues (2026-10-25, night)

Two rounds on an Apple Silicon Mac: first the "erratic stepping" of the Zig
0.16 `generator` (a fiber-based coroutine demo), whose causes and fixes are
below; then, since the first real target hit this many issues, a sweep over
everything that routes through platform code, done as six parallel efforts
with their own sections and scripted checks: the converter conformance corpus
(74 Mach-O binaries, "Converter conformance corpus"), the torture driver
ported to macOS ("Tests"), the scripted feature-flow suite (42 scenarios,
"Scripted feature-flow suite"), IPC / dyld notification / `--pid` routing
(this section), the arm64 unwinder ("Unwinding"), and the demon's process
control (detach, fork, exec, signals, frozen threads, thread exits; "Demon"
and below). Everything runs from the command line against private debugger
instances on ports of their own; the user's default port 7423 is never used.


Confirmed on an Apple Silicon Mac with a Zig 0.16 target (`generator`, a
fiber-based coroutine demo): building, launching, Retina scaling,
breakpoints, single steps and line stepping, in debug and `ReleaseFast`
builds. The "erratic" release-build stepping was reproduced on hardware and
had three causes, all fixed:

* **Stale `.rdi`.** Zig installs executables with the modification time of
  its cached copy, so switching a release build back to a debug build at the
  same path leaves an `.rdi` *newer* than the executable, describing other
  code; the user's debug binary was being stepped with a `ReleaseFast` line
  table. The converter now records the Mach-O `LC_UUID` in the RDI's top
  level info (`rdi_from_dwarf.c`), and `dbg_info.c` reconverts when it does
  not match the executable's (`macho_uuid_from_file_path`), or when the
  `.rdi` is older than the running debugger binary (so converter fixes reach
  existing `.rdi`s). Deleting `.rdi`s by hand is no longer needed.
* **Inlined code.** `DW_TAG_inlined_subroutine` was a TODO in the converter,
  so inlined code kept the inlinee's file and line in the unit's line table,
  and stepping hopped across `start.zig`, `Environ.zig`, `Thread.zig`, ...
  in release builds (where Zig inlines `main` into `start.main`). The
  converter now produces inline sites (name from the abstract origin, scope,
  own line table) and redistributes each unit's line rows (see "produce line
  tables from line rows & inline sites" in `rdi_from_dwarf.c`): the unit's
  table carries the call site's line across inlined code, like PDBs, and
  each site's table the inlined function's lines; nesting follows the tag
  tree, and range containment only finds the innermost site at an address
  (compilers emit nested sites whose ranges leave their parents'). Also
  fixed there: scopes with `DW_AT_ranges` were keyed by vaddr instead of voff
  (invisible on ELF, where the base is 0), and variables of inlined code,
  which only have an abstract origin, now get their names and types.
* **Stepping and frames with inline sites.** The engine stepped by the
  unit's line (so a step in `start.main` stepped over all of `main`) and the
  UI showed the outermost frame. Now `d_lines_from_dbgi_key_path_voff` and
  the call stack walk all scope ancestors (DWARF puts lexical blocks between
  nested sites); the trap net steps the line of the *selected* inline frame
  (`D_CmdParams.inline_depth`, from `rd_base_regs()`; the generic step
  command re-dispatches it); step-into also traps at the first instruction of
  every call inlined into the line, so inlined calls are entered like real
  ones; on a stop the UI selects the innermost inline frame (computed from
  the stop event's address: the cached registers may still be the previous
  stop's), except that a step over / out which finished in the procedure it
  stepped keeps the frame it stepped (a step over ending at the first
  instruction of an inlined call shows the call site; a step into from there
  selects the next frame without running). `raddbg --ipc state` reports
  `unwind_count` and `inline_depth`.

Also fixed while at it: `make_directory` creates missing parents on macOS
(the log folder `~/Library/Logs/raddbg/logs` was never created, so no logs
were written), and `--ipc` commands no longer lose option-provided registers
(`add_breakpoint --file_path:<path>:<line>` works; the empty primary
argument used to reset them).

Verified by driving the debugger from a script (`raddbg --ipc
launch_and_step_into`, `step_over`, `step_into`, `state`, with a scratch
`--project:` / `--user:` / `--logs:`); the `ui_thread.raddbg_log` records
each step's line and inline depth (`step_over_line:` blocks, `step ...:`
lines), the `ctrl_thread.raddbg_log` the demon events. Mind that the IPC
command runs with the focused tab's registers: once a disassembly tab is
focused (the UI opens one when a stop has no source), `step_over` steps by
instruction (`prefer_disasm`).

Follow-up the same evening, after "repeated step-into in the generator misses
parts of the execution; calls to `next()` after the first jump straight to
`contextSwitch`" (fixed, verified on both builds):

* `d_lines_from_dbgi_key_file_path_line_num` turned a source line map's
  addresses into ranges with the *unit's* line table, so for an inlined line
  the "other ranges of this textual line" were the call site's rows: the
  stepped range was extended over the whole inlined region and a step ran a
  whole loop iteration. `d_parsed_line_table_from_voff_line_num` now reads the
  range from the shallowest table whose row there is the requested line (the
  unit's, then the inline sites' outermost first), and natural-flow traps are
  never placed at the instruction being stepped from.
* A step over a line containing a coroutine context switch (`mov sp, xN; br
  xM`, inlined into `next()` / `yield`) ended at the jump's destination, in
  the other coroutine. Indirect jumps that are not calls or returns now get
  `D_TrapFlag_EndSteppingUnlessStackChanged`: the control thread ends the step
  after the single step only if `sp` is still near the step's starting stack;
  otherwise it keeps running, with the starting `sp` restored as the value the
  line's traps are checked against, so the step completes when the thread is
  switched back to. Stepping over `rng.next()` now runs the fiber and returns
  to the next line; stepping over the `for ... yield(i)` line runs the loop.
  Still imperfect: a step over such a line in a coroutine driven by *another*
  coroutine of the same type (range2d's `x_rng` and `y_rng`) can end in the
  parent after the first yield, since the sibling executes the same code
  while the parent runs.
* A breakpoint on a call that was inlined stops at the first instruction of
  the inlined callee; the stop now selects the frame of the breakpoint's own
  line (so main.zig:57 shows main.zig:57, with `next` as the frame below), and
  the find-thread retry and auto-unwind no longer override a selected frame.
* Wake-ups from other threads (a stop on the control thread, an IPC message
  on the socket listener thread) posted a Cocoa event from that thread, which
  does not wake a main thread blocked in `-nextEventMatchingMask:untilDate:`;
  stops and IPC commands were serviced seconds later, whenever something else
  woke the loop. `mac_plat_send_wakeup_event` now posts from the main queue.
  The IPC listener sets `SO_REUSEADDR` (a restarted debugger could not bind
  while old connections lingered) and messages that arrive before the first
  window exists are run and answered instead of dropped.
* The *first* `--ipc` reply of a fresh instance no longer stalls. The earlier
  10 s wait came from the listener rebind and the main-loop wakeup (both fixed
  above), not a misdelivered reply: instrumenting the fallback `connect` in
  `sock_async_tick` showed it never fires, and the reply always goes back on
  the accepted socket (~0.1 s). The connection lookup was hardened anyway so
  the endpoint stored on accept and the one reconstructed on recv/send are
  byte-identical for every address family: `SOCK_Endpoint.kind` is set on
  accept and carried in the ring header's protocol word, so the hash and
  `MemoryMatchStruct` agree (`mac_socket.c`). A reply larger than the
  listener's recv chunk is also reassembled by the `--ipc` client now, so
  `state` with a full module list is no longer truncated (`raddbg_main.c`).
  Verified by `tests/ipc_first_reply.sh` (10 fresh instances, first `state`
  under 1 s each).
* `raddbg --ipc --pid:<pid>` routes to a specific instance (used by
  `raddbg/tests/raddbg_tests.c`). Each instance additionally listens on a port
  derived from its own pid (`rd_ipc_port_from_pid`, `raddbg_main.c`), and a
  client with `--pid` derives the same port; the default port 7423 is
  untouched, so interactive clients are unaffected
  (`tests/ipc_pid_routing.sh`).
* Dynamic library load/unload events now arrive (previously the demon logged
  "could not find _dyld_debugger_notification ... load events will be
  synthesized at stops" and only the main executable and dyld appeared, so
  frames in shared-cache modules had no module and no unwind info). Two causes,
  both in `mac_demon.c`: the symbol `_dyld_debugger_notification` does not exist
  in this macOS' dyld (the notifier is `_lldb_image_notifier`), and the
  `dyld_all_image_infos` address captured at launch goes stale after dyld hands
  over to its shared-cache copy. The demon now prefers the published
  `notification` pointer and re-queries `TASK_DYLD_INFO`, reading whichever of
  the two candidate structs lists more images. After a stop in `main`,
  `ctrl_thread.raddbg_log` and `state` modules list libSystem.B.dylib,
  libsystem_kernel.dylib and the rest, and a dlopen/dlclose produce LoadModule /
  UnloadModule events (`tests/dyld_module_events.sh`). Those modules have no
  DWARF; their RDIs now come from the Mach-O symbol table (see "Symbol tables
  of shared-cache images" under Demon): `eval write` resolves, a function
  breakpoint on `write` hits and the frame is named
  (`tests/dyld_module_events.sh`). Note that stdio writes through
  `write$NOCANCEL`, so a `write` breakpoint only hits programs calling
  `write` themselves.
* Driving the debugger from a script: `launch_and_step_into` sent over `--ipc`
  has no target in its registers and launches nothing; send `step_into` with
  no process instead. `exit` over IPC now quits an instance even with a live
  debuggee: the external-driver path force-confirms the "attached to a live
  process" popup and tears the debuggee down like the GUI's close
  (`tests/ipc_exit_live.sh`). A `step_into` at the first instruction of an
  inlined call only selects the next inline frame, without a run. Each IPC
  `state` renders a frame, so poll at 100ms or slower. The harness used
  (`h.sh`: launch, breakpoint, run, step, printing every stop with its inline
  frames) runs a breakpoint-and-steps scenario in about a second.

* Frame selection after a stop is done by the stop handler itself (base
  registers set from the event's address, one FindThread) rather than through
  SelectThread + SelectUnwind, which needed the call stack within 10 ms and
  otherwise kept the innermost frame at random; a finished step is shown in
  the frame it stepped while the thread is still inside it, identified by the
  chain of inline-site scopes at the step's start and end (common prefix),
  not by a depth number, so leaving an inlined frame into a sibling inlined
  call shows the call site (`rd_stop_inline_depth_from_event`). The line
  lookup by file and line also matches the file, same-line ranges strictly
  containing the stepped range (an enclosing call site of the same line
  number: a generic instantiated within itself) are ignored, and the stepped
  line's own range is always in the trap net (at an inlined call whose first
  instructions carry the call line, the shallowest-table lookup returned only
  the enclosing range, and a step over at the inlined frame ran to the next
  breakpoint). The call stack's inline frame labels skip lexical blocks
  between sites, like the engine's own walks.
* Named POSIX semaphores and shared memory objects a debugger process creates
  (two semaphores and one object per instance, named by pid, for the debug
  info conversion) are unlinked at exit; they persisted until reboot and ran
  into `kern.posix.sem.max` after a few thousand launches, after which debug
  info never loaded. Secondary threads get an 8 MB stack (macOS default 512
  KiB).

Demon review fixes (2026-10-25, night; `src/mac/tests/demon/run_all.sh`, one
script per item, each building a small C debuggee and driving a private
instance through `tests/ipc_harness.sh`): detach never completed (the process
was released before the engine's wait for its `ExitProcess`; and `PT_DETACH`
needs `SSTOP`), forked children spun forever on inherited `brk`s, a passed
hardware exception's signal was re-reported instead of delivered (so `SIGSEGV`
handlers never ran), frozen threads had their pending exception answered with
another run's ignore flag, a reported thread's exit went unreported when a new
thread's exception arrived first, exec was not handled (and the post-exec
image would have had the old traps restored into it), plus the small items
(exception-port install failures, watchpoint attribution, a leaked reply
right, the `EVFILT_PROC` registration, the fault address). What the checks
showed and what they could not: `detach.sh` (the Zig generator detached at a
breakpoint runs to completion, the debugger reports no process within a
second; with a second process attached by pid the other stays known and
runnable), `fork_child.sh`, `signal_handler.sh` (`recovered` printed, one
first-chance event, clean exit), `frozen_thread.sh` (`select_thread` cannot be
driven over IPC, so the frozen-thread situation is produced by the run which
reports a second thread's held-back breakpoint hit while the faulting thread
is frozen; continuing stops on the fault again instead of a dead process),
`thread_exit.sh`, `exec.sh` (into a binary of our own; `/bin/echo` is run for
information: see Exec above), `watchpoint.sh` (hit count 1 in a saved copy
of the project).

Exception stops are enabled by default now (they never were from the UI:
`raddbg.mdesk`'s project schema had `win32_*` and `linux_*` "Break on ..."
settings but no `mac_*` ones, so every Mach exception and signal was passed at
first chance). The schema has one `mac_*` setting per `MAC_ExceptionCodeTable`
entry (`mac_` + the lower-case name, the table's `break_default`), which the
UI reads into the engine's exception filters exactly like the Windows and
Linux ones (`raddbg_core.c`, `exception_code_filters`; nothing in the engine
or demon changed). A NULL store now stops at its first chance as
EXC_BAD_ACCESS (0x10001), `__builtin_trap` as EXC_BREAKPOINT (0x10006),
`abort()` as SIGABRT (0x6) and `raise(SIGINT)` as SIGINT (0x2), the last two
inside `__pthread_kill` with `main`'s line further down the stack. A continue
after such a stop follows upstream: the engine runs with
`ignore_previous_exception`, which the demon answers with KERN_SUCCESS for a
hardware exception (the instruction re-executes and faults again, a second,
repeated stop at the same pc) and with `PT_THUPDATE` signal 0 for a soft
signal (not delivered: `raise(SIGINT)` returns and the process exits 0), as
the Linux demon's `PTRACE_CONT` with signal 0 and Win32's `DBG_CONTINUE` do;
the exception reaches the debuggee (a `SIGSEGV` handler runs, a crash
terminates) when its setting is off. So `tests/demon/signal_handler.sh` turns
the three fault settings off on purpose, `frozen_thread.sh` turns `mac_sigusr1`
off for its pass-through thread (SIGUSR1 breaks by default, as on Linux), and
the flows `exceptions`, `U2` and `D4` pin the codes and both continue paths.
Two more consequences of the Linux-like defaults: `raise(SIGUSR1)` stops
before its handler runs (the `signal_handler` flow turns `mac_sigusr1` off),
and a `kill -9` of the debuggee is reported first as a SIGKILL stop (0x9;
`PT_ATTACHEXC` reports it as a soft signal and `mac_sigkill` breaks, as
`linux_sigkill` does), the exit following the continue (`D9`; a launched
process then reports exit code 9, an attached one 0 on this machine with or
without the stop, since the demon learns a non-child's exit status only from
`NOTE_EXITSTATUS`: `D9_exit_code_is_signal` stays open).

### Open

Consolidated from the sections below (each records its evidence); roughly by
impact:

* Shared-cache images: function starts with no symbol at all (19 in
  libsystem_c; lldb shows them as `___lldb_unnamed_symbol<N>`) end the
  preceding procedure and are themselves unnamed, so a stop inside one is
  `???`; `eval` of a symbol-table procedure prints an empty value (it has no
  type). The locals themselves are present (Demon, "Local symbols").
* Fixed (see "Unwinding", function starts): stopped at the entry of a frameless
  syscall stub (`write`), at a function's first instruction, or in a `__stubs`
  entry, the call stack skipped the caller; the engine now hands the unwinder
  the function's start from the RDI procedure, and stubs are recognized by
  their code. Still open there: a step over after such a step out lands on
  the middle of the call's line, and the next step over stays on it (the
  stepping item below, `U5`; `stepping_O0`'s `into_strlen_skipped`,
  `libsystem_len` and `at_main_locals` now fail that way rather than in
  `main`). `D3`'s `write` breakpoint cannot hit on this macOS: `basic.c`'s
  `printf` reaches `write$NOCANCEL` (`__write_nocancel`), not `write`; the
  scenario needs a direct `write()` call, or `__write_nocancel` as the name.
* **Step-into and code without line info.** Every Zig `threadlocal` access
  calls dyld's TLV getter through an indirect `blr`, and a step into on such
  a line lands there (the trap net cannot know an indirect call's destination
  up front; direct calls to functions without line info are skipped unless
  they are the line's last call, as upstream does). A step from there is an
  instruction step, and the UI opens disassembly. The right fix is in
  `d_ctrl_thread__run`: give those traps a flag meaning "after the single
  step, end stepping only if the new pc has line info, else trap at the
  return address (`lr`, or `[sp]` on x64) and keep stepping" (the module ->
  `d_dbgi_key_from_module` -> `di_rdi_from_key` -> `rdi_line_info_idx_from_voff`
  lookup is available on that thread). Related: a step into on a line calling
  through a `__stubs` entry (`strlen`, a weak template instantiation) lands on
  the stub, and a step over the stub's indirect jump runs to the process'
  end; a step over a tail call (`b target` at -O2) ends in the callee's first
  line (`stepping_O0`, `stepping_O2`). Step out of a signal handler, and a
  step over `longjmp`, run to the process' end.
* `step_out` from an inlined function steps out of the concrete function; it
  could use the selected inline site's ranges (`step_out_scope` already works
  from scope ranges). A step over at inline depth 1 from a line into which
  LLVM scheduled a helper's instruction stops at that instruction (flows
  `F1`/`F2`); a breakpoint on an inlined line stops first in whichever
  instance comes first in the code (`F4b`).
* Frame names of outer frames come from the raw return address (the UI and
  `d_call_stack_from_unwind` look symbols and scopes up at pc, not pc-1), so
  a frame whose call does not return (`fn a() void { @panic("x"); }`) is
  named after the function which follows it in the binary; the unwind itself
  is right (`state`'s `call_stack` already looks callers up at pc-1).
* Evaluation (the fixes are under "Scripted feature-flow suite", "Evaluation
  fixes"): over `--ipc`, the first `eval` touching a memory page the frontend
  has not read yet answers from nothing (an empty string for a `char *`, 0 for
  a number) until the asynchronous page read lands, and the first `eval` after
  a stop can answer from the previous stop's copy of a page (the frontend's
  eval waits a frame's spare time for a page it has never read and takes a
  page it has as it is; the `watchpoint` NOTEs, the TODO at `RD_CmdKind_Eval`);
  `eval` of a global right after a stop can answer "could not be found" for a
  few hundred ms (the name search is asynchronous); the first `eval` of
  another thread's call stack can come back empty while it is being built; a
  stop in a thread created after another exited evaluates in the main thread's
  context (`D7`). In an outer frame only the callee-saved registers (x19-x28,
  fp, lr, sp) are the frame's own: a local held in another register (an
  argument register at -O2) reads the inner frame's value, since the unwinder's
  register blocks carry no availability (lldb shows those as unavailable).
  `(uint64)&g` parses as `uint64 & g` and reads 0 (the evaluator's C-style
  cast rule gives up when a prefix operator follows; use `cast(uint64)&g` or
  `(uint64)(&g)`); `&g` prints its pointee over `--ipc` (`disable_addresses`).
  At a watchpoint stop `state`'s `ip` is the stop event's address, the
  accessing instruction, while the thread's registers (`pc`) are one
  instruction past it (the demon steps the thread past the access after
  reporting it). A watchpoint's condition is not evaluated (`D8`): the run
  loop gathers conditions for traps whose `vaddr` equals the event's
  instruction pointer, and a watchpoint's trap carries the data address (the
  event's `address`); `event->user_data != 0` then stops unconditionally.
* IPC: `save_project` without `--file_path:<path>:<line>` takes the focused
  tab's source file as its destination, and a second call while the overwrite
  confirmation is up goes through it; a FilePath register is only filled from
  a `path:line` string; option values cannot contain spaces; outputs pushed by
  a chained command are often lost (double-buffered command queue), so check
  effects via `state` or the logs.
* Demon: exec of a platform binary (`/bin/echo`) kills the traced process with
  `SIGTRAP` (the OS drops the exception ports; lldb behaves the same); a
  forked child which hits a trap written by an earlier run (a breakpoint since
  removed) takes `SIGTRAP`; the run's single `ignore_previous_exception` flag
  applies to every thread it resumes (a passed signal of a thread resumed by a
  later run is dropped).
* Stepping over a coroutine context switch in a coroutine driven by *another*
  coroutine of the same type (`range2d`'s `x_rng` and `y_rng`) can end in the
  parent after the first yield, since the sibling executes the same code
  while the parent runs. Optimized code attributes scheduled instructions to
  the function's declaration line (`start.zig:641` fragments between other
  lines); stepping visits them, as other debuggers do. After a step out onto
  the middle of a line, the line's remaining rows count as a new line (`U5`).
* Converter: inline sites keep the inlinee's bare name (`area`; procedures are
  qualified through their containers, `geom::Rect::area`); an anonymous
  namespace or class contributes an empty part to a qualified name
  (`main::::operator()` for a lambda); `RDI_Member` has no bit fields, so a
  bitfield reads as the byte holding its first bit with its storage unit's
  width; code the line program attributes to an inlinee's line before its site
  begins, or in a row of its own after it ended, keeps that line in the
  enclosing table (the DWARF's attribution; see `RESULTS.md`); an RDI made
  from the symbol table alone is marked (`[symbol table only]` in its
  producer string) but `dbg_info.c` does not yet treat it as stale once a dSYM
  or the debug map's objects turn up.
* `dylib_host_O0` exits 0 without its final `printf` under the debugger
  (natively "host done 8", exit 8) after the second `dlclose`. `eval signo` at
  the first line of a signal handler is flaky (0 or 30).
* A headless driver (the demon from a console program, the way `torture`
  does) would still be useful; the `--ipc` loop is the next best thing.

## Converter conformance corpus (2026-10-25, night; 2026-10-26)

`src/mac/tests/macho_corpus/` holds a Mach-O test corpus and a checker for the
DWARF → RDI converter; `RESULTS.md` there is the table of the last run (with a
hand-maintained findings section at the end). 74 of 74 pass.

* `build_corpus.sh [-o <dir>]` builds the matrix into a directory outside the
  repository (default `$TMPDIR/raddbg_macho_corpus`): C at `-O0`, `-O2`,
  `-O3 -fomit-frame-pointer`, `-flto`, `-gdwarf-2`, `-gdwarf-3`, `-gdwarf-4`,
  with relative source paths, and with `DW_AT_call_file`/`DW_AT_call_line`
  stripped from the inline sites (assembly rewrite); C++ (templates, virtual
  calls, exceptions,
  lambdas) at `-O0`, `-O2`, `-flto`; a dylib with an executable linking it and
  one `dlopen()`ing it; a static library (`N_OSO` into the archive); threads
  with `_Thread_local`; large frames / VLAs / `alloca`; deep recursion; signal
  handlers; an arm64+x86_64 universal binary; the Zig generator in Debug,
  ReleaseSafe and ReleaseFast (`zig build --prefix ... --cache-dir ...`, the
  project is not touched); Rust (`split-debuginfo=unpacked`) and Swift. Every
  entry comes twice: with a `.dSYM` next to it, and as the bare executable with
  only its debug map, so that `radbin` runs `dsymutil` itself. Two `symtab-*`
  entries are `strip -S` copies (of `cprog-O2` and `libshape.dylib`): no debug
  map, no DWARF, converted from `LC_SYMTAB` alone.
* `python3 -I check_corpus.py [--build-dir <dir>] [--results RESULTS.md]`
  converts every binary with `build/radbin --rdi` and validates `radbin --dump`
  against `dwarfdump --debug-line` / `--debug-info` / `--uuid` of the dSYM (its
  own `dsymutil` run for debug-map entries) and `otool -l`: the guid is the
  `LC_UUID`; `voff_max` and the binary sections cover the text; one unit per
  CU; every `is_stmt` row is covered, with its line and file, by the unit's
  table or, inside a `DW_TAG_inlined_subroutine`, by the innermost site's,
  while the enclosing tables carry `DW_AT_call_line`; tables are sorted,
  non-empty, inside the image and inside a DWARF sequence; procedures, lexical
  blocks and inline sites match the DIE tree structurally (ranges as voffs,
  names, nesting); procedures carry the fully qualified name of their
  declaration (`geom::Rect::area`, `rustprog::{impl#0}::area`, from the
  declaration's namespace / class parents; modules are transparent); inline
  sites and procedures have function types; source line maps agree with the
  tables; file-scope globals and thread-locals exist with their addresses;
  named struct / class / union / enum definitions have UDTs with the DWARF's
  member offsets (bitfields included), enumerator values and array counts
  (`types`); the stripped copies' procedures are the `nm -n` code symbols
  with the ranges up to the next symbol (`symtab`); on the C debug-map
  entries a second `radbin` run reuses the generated dSYM and an empty file
  planted at the cached DWARF path is removed and regenerated (`dsym_cache`).
  74 binaries take about 30 s; the largest (the Zig Debug generator, 2.7 MB
  of DWARF) converts in 0.1 s.

  `is_stmt`: rows keep the flag; a row which is not a statement continues the
  line before it in each table it lands in, a statement row begins a new entry
  when its line or file differs. The checker verifies against
  `dwarfdump --debug-line` (Apple's llvm-dwarfdump) that every statement row is
  covered with its line and file by the right table, that the first entry of a
  site's table is the line of the DWARF row at the site's start (statement or
  not), that a statement row changing the line begins an entry at exactly its
  address and a non-statement row begins none (except at a site's or a
  sequence's start). On the Zig Debug generator this covers 42740 statement
  rows, 8700 of them inside the 2358 inline sites; no statement row's line is
  lost at a site start.

Found by the corpus and fixed (`dwarf_parse.c`, `rdi_from_dwarf.c`,
`macho_parse.c`, `radbin.c`):

* `.debug_rnglists` / `.debug_loclists`: a `DW_RLE_base_addressx` (and
  `base_address`) entry pushed an empty range at address 0, so every scope
  with `DW_AT_ranges` in clang's DWARF 5 (dsymutil begins each list with one)
  carried `[-base, -base)`, the scope vmap reached `0xffffffff00000000`, and
  multi-range lexical blocks and inline sites did not match their DIEs.
* Line table file names: an absolute DWARF 5 file name was appended to its
  include directory (`<comp_dir>/<absolute path>`); entry 0 names the primary
  file in full and clang's rows use it whenever the source lies outside the
  compilation directory, so the debugger could not find the source.
* `DW_FORM_ref_addr` references resolved to 0 (dsymutil's ODR uniquing turns
  `DW_AT_specification` / `DW_AT_abstract_origin` into them, LTO too), and
  `DW_AT_specification` was never followed: C++ member function definitions
  and Rust methods were nameless procedures, inline sites of libc++ and LTO
  builds were named `???`, static data member definitions were dropped. The
  origin chain now follows both, across units, with the owning unit's context.
* Variables under namespaces, classes and modules (Rust statics and
  `thread_local!`, Swift globals, C++ class statics) had no location: the
  frame-base iteration ran zero times for a parent without frame-base cases.
* DWARF ≤ 3 location expressions (`DW_FORM_block1`) were ignored (`-gdwarf-3`:
  no global or TLS locations); `DW_AT_high_pc` of address class was not
  rebased.
* From the converter review: inline sites carried the inlinee's return type
  instead of its function type; abstract instances became rangeless
  procedures; the single-location sentinel range was rebased; `is_stmt`
  merging at decode time could carry the caller's line into the first entry of
  a site's table (rows now keep the flag and extend lines per table); sites
  without a usable call site (Swift's `/<compiler-generated>`, line 0) left
  holes in the enclosing tables; a row running past a nested site now gives the
  enclosing site's table the call line below it.
* `radbin`'s generated-dSYM cache was keyed by `LC_UUID` only; ld64 hashes the
  image contents, so the same code with other debug info (`-gdwarf-3`, other
  source paths, a comment added) shared an entry. The key now includes a hash
  of the debug map's object paths and modification times (`MACHO_Bin.debug_map_hash`).

Second round (2026-10-26; `RESULTS.md` has the table of bugs 7-12):

* Swift functions directly under `DW_TAG_module` had no function type (56 of
  84 procedures of `swiftprog-O`, and 60 of their 268 inline sites): the type
  pass gathered the children of namespaces, UDTs and functions only, and a
  module's subtree was skipped through its sibling link. Zig's nested types
  (`Io.Clock.Duration` under the enum `Io.Clock`, `Target.Cpu.Feature.Set`
  under a zero-sized base type) were missing for the same reason, their
  members' types nil. The children of every tag are visited now.
* UDTs took every `DW_TAG_member` below their DIE, the members of nested types
  included (Zig's `Target.Cpu` had `Target.Cpu.Model`'s `name`, `llvm_name`,
  `features` as members of its own); only the direct children are members.
* C++ member functions defined out of line were named without their class
  (`area`): a symbol's container came from its DIE's parent, never from the
  declaration's. The declaration's parents (recorded with each tag's hash,
  which also fixes the one-level namespace chain: `std::__1`, not `__1`, and
  nested UDTs' containers) qualify definitions reached through
  `DW_AT_specification` / `DW_AT_abstract_origin`: `geom::Rect::area`,
  `geom::Shape::s_live_count`, `rustprog::{impl#0}::area`, Swift `Rect::area`,
  as `fully_qualified_str8_from_rdi_symbol` prints them. The in-class
  declaration of a static data member no longer becomes a second,
  location-less global.
* Rows are no longer merged when decoded: a statement row repeating the line
  of the row before it, beginning at an inline site's start, began outside the
  site, so the code it covered past the site's end kept the inlinee's line in
  the enclosing table (Rust `dealloc`). Contiguous pieces of one line merge
  per table when the tables are built, which gives the same tables everywhere
  else.
* From the second review: `D2R_InlineRange` parent linking under partially
  overlapping ranges, no inline sites inside abstract instances, bitfield and
  `DW_OP_plus_uconst` member offsets, `DW_FORM_ref_addr` address-sized in
  DWARF 2, no column data in line sequences (the baker expected pairs), dead
  counters removed; `radbin`: dsymutil links into `<bundle>.tmp.<pid>` and
  the bundle is published by a rename, a cached bundle is reused only when its
  DWARF file is a Mach-O with the executable's UUID (else removed and
  regenerated), `Foo.app.dSYM` next to an application bundle is found, RDIs
  made from `LC_SYMTAB` alone carry `[symbol table only]` in their producer
  string.

Open: locals (locations, types) and type byte sizes are not checked; see the
Converter bullet of "Open" above.
## Tests

Demon checks: `zsh src/mac/tests/demon/run_all.sh` (settings in `common.sh`:
`IPC_PORT`, default 7950, `HARNESS_DIR`, `RADDBG`; `detach.sh` needs the Zig
generator project and `zig`, see its header). Each script builds its debuggee
with the host clang (two steps, so that the object file the debug map points at
survives; the debugger's converter then reads the dSYM), writes a project file
(the `// raddbg ... project file` header line is required, or the file is taken
for a new, empty project) and asserts on `--ipc state`, the debuggee's
`stdout_path` file and `ctrl_thread.raddbg_log`.

`./run_tests.sh clang debug` (or `release`) builds `raddbg`, `raddbg_non_graphical`,
`radlink`, `radbin` and `torture` with `./build.sh meta ...` and runs `build/torture`
over every registered test; macOS is clang-only. Arguments after `--` go to torture,
e.g. `./run_tests.sh clang debug -- -keep_going -ipc_port:7705 '*'`. By hand:

```
./build.sh meta clang debug raddbg raddbg_non_graphical radlink radbin torture
cd build && ./torture -clang:/usr/bin/clang -keep_going -ipc_port:7705 '*'
```

The `raddbg/*` tests launch `raddbg_non_graphical` instances and drive them over IPC;
those instances listen on `-ipc_port:<n>` (default: 7700 + torture's pid modulo 100), so
a debugger open on the default port is left alone. Each test's own user and project
files, the debugger's console output and the built debuggees land in
`build/torture_artifacts/<test>/`.

The prebuilt test inputs (`local/test_data/inputs`, fetched by `run_tests.bat` from
data.raddbg.com) are Windows executables and PDBs, so on macOS the stepping regressions
run over `src/mule/old_test_debuggees/{basic_stepping_test,step_into_for_goto}.c`,
compiled on the fly with the host clang (`-g -O0`, Mach-O + dSYM). They launch with
`step_into` (stop at the entry point, `main`), then alternate `step_into` / `step_over`
and log every stop (lines, voff, symbol) until the process exits; the log is compared
with `local/test_data/exemplars/<test>/exemplar_mac`, which the first run creates (and
`eval2_regressions` writes `exemplar_mac_arm64`). Inspect a new exemplar before trusting
it: `basic_stepping_test_step_regressions_0` records main:49, 51, foo:33, 35, main:51, 55,
then a stop at voff 0x4ac without lines, which is the `_printf` stub (`__stubs`): a step
into the line's last call enters the stub, and the step out from there returns one
frame too far (see "Step-into and code without line info" above), so the process runs
to its exit. `step_into_for_goto` records 18, 19, 21, 22, 20, 24 (clang attributes the
loop's back-jump to the `}` on line 22).

State on 2026-10-25, debug and release builds (158 registered tests): 144 pass, 13
skip, 1 fail.

* Skipped: the four `mule_main_9ff1e58f_*` stepping tests and the `p2r_*` / `d2r_*`
  converter tests (no test data on this machine; `run_tests.sh` does not download it),
  `mdesk/tokenizer` and `d2r_regressions` (skipped upstream), and the four linker
  tests that need MSVC (`icf_cpp_*` compile with `cl.exe`, `icf_llvm_addrsig*` link
  `libcmt.lib`): `T_SkipWithoutCl()` in `torture.h`.
* Failing: `linker/default_lib_matches_explicit_path`. radlink parses its command line
  with link.exe rules, so an input given as an absolute POSIX path is taken for a
  `/option` and dropped; the test's second iteration passes the library as
  `/.../sdk libs/provider.lib`. Reproduce in `build/torture_artifacts/default_lib_matches_explicit_path`:
  `../../radlink /subsystem:console /entry:entry /out:default.exe entry.obj /include:first_provider "$PWD/sdk libs/provider.lib" /defaultlib:provider.lib`
  (unresolved `first_provider`); the same command with the relative path links.
* Fixed to get here: an absolute POSIX path as the command-line target was treated as a
  `/flag` and never became a target (`rd_init`); `torture` captured child output with
  `pipe2` and pidfds (now kqueue on macOS), looked for `cl.exe`, and had no mac
  tables; `lnk_file_rename` used `/proc/self/fd` (now `F_GETPATH`); the first run of
  an exemplar test compared against a missing exemplar instead of creating it.

## Scripted feature-flow suite (src/mac/tests/flows)

`src/mac/tests/flows/run_flows.sh` drives a private debugger instance (its own
IPC port, user file and logs; never the user's port 7423) through the feature
flows against small Mach-O debuggees and prints `PASS`/`FAIL` per check with
the expected and observed values (file:line, symbol, inline depth, values, exit
codes), plus `NOTE` lines for observations without a verdict.

* Debuggees: `src/mac/tests/flows/debuggees/*.c|cpp`, built by
  `build_debuggees.sh` into `build/flows` (`FLOWS_BUILD_DIR`) as `-g -O0` and
  `-g -O2` variants with dSYM bundles (compiled and linked separately: a
  one-step `clang -g x.c -o x` deletes its object before `dsymutil` can link
  the dSYM, leaving only a debug map), plus `basic_O2_nofp`
  (`-fomit-frame-pointer`), single-variant debuggees for the review scenarios,
  `big_symbols` (generated by `build_debuggees.sh` into `build/flows`: 20k
  functions and struct types, ~19 MB of DWARF, so that its conversion takes
  seconds; `bp_resolution_converting`) and the Zig generator (debug build;
  `--prefix`/`--cache-dir` under `build/flows`, the generator's directory is
  untouched). Lines the scenarios refer to carry `// @tag` comments; the
  runner looks the numbers up by tag.
* Run: `zsh src/mac/tests/flows/run_flows.sh` (all, ~7 minutes),
  `run_flows.sh -l` (list), `run_flows.sh stepping_O0 eval_O0` (some).
  `HARNESS_DIR` keeps the project files, the user file and `logs/`
  (`ui_thread.raddbg_log`: trap nets; `ctrl_thread.raddbg_log`: demon
  events); `FLOWS_KEEP_LOGS=1` copies each scenario's logs and the debuggee's
  output to `$HARNESS_DIR/logs_<scenario>/`. Scenario names: plain = feature
  flows, `F*` = stepping/inline-frame review, `D*` = demon review, `U*` =
  unwinder review.
* `--ipc` additions for scripting: `state` reports `processes` (pids),
  `exited_process_count` / `last_exit_code`, `selected_thread`, each thread's
  `frozen` flag, the selected thread's `call_stack` (per frame `unwind_count`,
  `inline_depth`, `ip`, `module`, `symbol` (callers looked up at the call),
  `ip_symbol` (at the frame's pc itself), `file_name`, `line_num`), the
  `breakpoints` (`id`, `location`, `condition`, `hit_count`, `enabled`) and
  the stop event's `bp_id`; `--process:<pid>` / `--thread:<tid>` options name
  control entities by the ids `state` reports (`kill --process:123`,
  `freeze_thread --thread:456`, `select_thread --thread:456`, `detach
  --process:123`); `--prefer_disasm:1` / `--force_confirm:1` set the boolean
  registers. Option values cannot contain spaces (`--string:i==3`).
* Scripting caveats: right after a stop `eval` of a global can answer "`x`
  could not be found" for a few hundred milliseconds (`di_match_from_string`
  searches the debug infos asynchronously and `eval` does not wait), and a
  value in memory the frontend has not read yet prints as 0 until the
  asynchronous page read lands (`eval` waits a frame's spare time at most,
  see the TODO at `RD_CmdKind_Eval`; the frontend's own views read the
  stopping thread's frame at an ordinary stop before a script gets to it, but
  not at a stop which reports a breakpoint hit held back from an earlier run,
  `threads_breakpoints` since the fix below); the runner's `ev` retries the
  former and accepts an answer only once two consecutive evaluations agree.
  `eval` over `--ipc` runs with `disable_addresses`:
  pointers print their pointee, address casts give 0, a function's address
  cannot be read (`&inner` prints the signature); use the stop's `ip` from
  `state`. Never `wait` on a shell background process the debugger attached
  to: `ptrace` reparents it to the debugger.

### Results and findings (2026-10-25, this branch)

Last full run (with the evaluation fixes below): 375 checks passed, 38
failed, 46 scenario runs in 235 s (the same tree before them: 361 passed, 39
failed; nothing that passed then fails now; before the breakpoint fixes: 316
passed, 68 failed). Every failure below is a
recorded bug or observation; scenarios fully passing include
launch/kill/restart/exit, the breakpoint flows (line, inline line, dylib
before load, address, condition, hit counts, toggle/remove, function name
without a warm-up), the four simultaneous worker hits, watchpoint, halt,
eval, the frame-pointer-less call stack, freeze/thaw, modules
(load/unload/reload), the Zig generator stepping and fiber stack, D7, D9, F3,
F4a, F5a, F5b, U1, U2, U3.

Fixed here (the suite found them; the first and third were fixed independently
on `macos-arm64-port` at the same time, and the merge kept that branch's
implementation):

* `--ipc` replies were cut at 4 KB: `mac_socket.c` hands received data out one
  `recv()` chunk at a time and the client in `raddbg_main.c` took the first
  chunk as the whole reply, so `state` with the system libraries loaded (after
  an attach) ended inside the module list. The client gathers packets until
  the header's size is met (see "IPC" above).
* All `T[N]` arrays of one element type became the first one seen
  (`int arr[3]` and `int g_array[8]` both `int [4]`, `sizeof` 16): the DWARF
  converter's type deduplication hashes each tag's attributes, but
  `DW_TAG_subrange_type` (where `DW_AT_count` lives) and `DW_TAG_enumerator`
  were not among the hashed tags (`rdi_from_dwarf.c`, `need_tag_hash`), so
  every array of `int` had the same tree hash. Both are hashed now; `.rdi`s
  rebuild automatically (older than the debugger binary).
* C++ member functions defined out of line were unnamed procedures (empty stop
  symbols and call stack entries for `Rect::area`, constructors, lambdas'
  `operator()`): their names, types and linkage names live in the declaration
  the definition refers to with `DW_AT_specification`, which the converter did
  not follow (only `DW_AT_abstract_origin`), and clang encodes it as
  `DW_FORM_ref_addr`, which `dw2_reference_info_off_from_form_val` did not
  decode at all (`dwarf_parse.c`). The origin walk now starts at the
  specification and follows `ref_addr` references into other units (the
  converter-corpus work above), and the declaration's class qualifies the name
  (`geom::Rect::area`; "Converter conformance corpus", second round).
* Soft signals neither stopped nor got delivered (`abort()` and `raise(SIGINT)`
  were filtered with `stop_cause: 0` and continued with
  `ignore_previous_exception`, so `raise(SIGINT)` returned and the process
  exited 0) and hardware exceptions stopped only on their repeated second
  chance: the `mac_*` "Break on ..." settings had no schema in `raddbg.mdesk`
  and read as off (see "Status and open issues"). With the schema, `exceptions`
  checks the first-chance stops (NULL store 0x10001 at its line, `__builtin_trap`
  0x10006, `abort()` 0x6 and `raise(SIGINT)` 0x2 in `__pthread_kill` with
  `main`'s line on the stack), `U2` the trap, and `D4` both paths of a
  `SIGSEGV` handler: with the fault settings off no stop, the handler runs and
  the process exits 0; with the defaults the first-chance stop, and a continue
  that stops on the re-executed fault again (the policy below). The
  `signal_handler` flow turns `mac_sigusr1` off so that `raise(SIGUSR1)` reaches
  its handler (SIGUSR1 breaks by default, as `linux_sigusr1` does), and `D9`
  continues once past the SIGKILL stop a `kill -9` now produces (`mac_sigkill`
  breaks by default) before expecting the exit; its exit code check (9) fails
  for the attached process with or without that stop (exit code 0: see "Status
  and open issues"), as do `fork/D2_exit_code_from_child` (0 instead of 7) and
  `D1_debuggee_ran_to_completion` on the pre-change binary too.
* Function-name (expression) breakpoints added while a process was live never
  got a trap when nothing had evaluated the name before
  (`breakpoints_O0/func_bp_cold_symbol`, `F5b big_frame`, the Zig `range`):
  the control thread resolves them through `e_eval_from_string`, whose
  identifier lookup polled the asynchronous debug-info name search the
  frontend's way, `di_match_from_string(..., endt_us = 0)`, and took "not seen
  yet" as the answer. The eval base context now carries a deadline for those
  lookups (`E_BaseCtx.dbgi_match_endt_us`, used by `eval_ir.c` and
  `eval_parse.c`); the control thread's eval scope sets it
  (`d_ctrl_thread__eval_scope_begin`), so breakpoint expressions and conditions
  wait for the search as they already waited for the RDIs, and the frontend
  leaves it 0 as before. The runner's `warm_symbol` work-arounds are gone.
  What the deadline costs (reviewed): the search (`di_match_artifact_create`)
  never waits for a conversion (it takes the RDIs as they are, with
  `di_rdi_from_key(..., endt_us = 0)`; conversions run in `radbin` processes,
  not on the async workers), so the budget is only ever reached with the async
  workers swamped, and only for a name no lookup has asked about yet (a match
  once computed is cached). It is one absolute time per scope (every lookup
  waits until the same instant, so a condition with several unknown names pays
  it once): 5 s after the scope began, or 1 s while some module's RDI is still
  converting (`di_rdi_parsed_nil_waiting`: names in that RDI cannot be found
  yet anyway). A match computed while an RDI converted is stale once the RDI
  loads (`di_load_gen`); the frontend takes stale matches as they are while a
  fresh one is computed, and the control thread used to as well, so a
  function breakpoint added during the conversion resolved one run late. The
  control thread now asks for a fresh match within the deadline
  (`E_BaseCtx.dbgi_match_wait_for_fresh`, `di_match_from_string_flags` with
  `AC_Flag_WaitForFresh`). `bp_resolution_converting` times this with a
  generated debuggee of ~19 MB of DWARF (`big_symbols`): six steps over with
  a breakpoint on an unknown name while it converts, each well under the 5 s
  budget, and a breakpoint on `big_fn_1` added during the conversion hits on
  the first run after it.
* Breakpoint hits on several threads at once were lost but one: the demon
  reports every thread's pending exception from one `dmn_ctrl_run` (four
  `Breakpoint` events for the `threads_breakpoints` workers), the engine stops
  for the first, and the other events stayed in its queue, where the next run's
  single-step runs consumed them as noise while taking "stuck" threads off
  their trap -- which the other hitters were, since arm64 reports a `brk` with
  pc at the instruction (1 of 4 worker hits); an event reaching the run loop
  instead would have carried a trap id into a released message. The end of a
  run now drops the other threads' queued `Breakpoint` events and marks their
  thread entities (`D_Entity.has_unreported_bp_hit`, `dbg_engine_ctrl.c`):
  such a thread is not stepped off its trap, and the following runs report one
  held-back hit each, through the regular breakpoint handling (conditions,
  thread filters), before resuming anything; a hit whose breakpoint is gone, or
  whose thread was moved, is dropped. The win32 demon gets the same hits one per
  run from the kernel's queue; nothing changes for it. All four workers now stop
  at the line, with all five threads alive at every stop.
* Evaluation fixes (the eval review; `eval_O0`, `F4a`, `F6` check them):
  - DWARF register 0 (x0 on arm64, rax on x64) mapped to the nil register:
    `arch_reg_code_from_dw` scanned the register table from index 0, whose
    entry (the nil register's) is 0 too, so every `DW_OP_reg0` / `DW_OP_breg0`
    location became a read of register 0 with size 0 (`radbin --dump` showed
    `RegRead(??? (0)+0, Size: 0)`), which the interpreter reads as 0 without
    an error: `F4a`'s `r` (in x0 after the recursion) read 0. The scan starts
    at 1 (`arch.c`); the unwinder's CFI expressions go through the same table.
  - `DW_OP_entry_value(...)`, `DW_OP_stack_value` (clang's location for an
    argument whose register was reused: `main::argc` after the call to `g`)
    was a TODO in the converter, leaving an empty bytecode stream which read
    as 0. An expression with an operation the bytecode cannot express now gets
    no location case at all (`rdi_from_dwarf.c`, `dst_bytecode_is_good`, which
    was set but never used), so the variable answers "Could not find location
    info" in that range, as for a range no location list entry covers; the
    value at entry cannot be recovered (it would take the caller's call site
    parameters). `DW_OP_stack_value` itself (the top of the stack is the value,
    not an address: `DW_OP_consts 10, DW_OP_stack_value` for `F6`'s `sum`,
    `DW_OP_breg29 -8, DW_OP_stack_value`) was not handled either and left the
    address flag of the preceding operation; it now marks the value.
  - A variable left with a single ranged location case (a one-entry location
    list, or one entry left after the above) lost its range in the bake
    (`rdi_make_local.c` baked a set only for two or more cases; `radbin --dump`
    showed `range: *always*` for `f::r`'s `[0x434, 0x440)`), so the location
    was read outside of it (a register which had since been reused). A lone
    case with a bounded range now bakes as a one-element set
    (`rdim_location_case_list_needs_set`, `lib_rdi_make`).
  - Chained prefix operators did not parse: `*&x`, `!!x`, `**pp`, `&*&x` and
    `cast(uint64)&g` answered "Missing expression." (the parser gathered one
    prefix operator, then expected an atom). Prefix operators now chain, applied
    innermost first (`eval_parse.c`). `(uint64)&g` still parses as `uint64 & g`
    (the C-style cast rule is upstream's: it gives up when a prefix operator
    follows the parenthesized type; `(x) * y` would be ambiguous).
  - Not bugs: `rec->name` reading `""` in a caller's frame while `(*rec).name`
    read `"rec"` was the first-read scripting caveat (whichever form is
    evaluated first pays for the page); `F6`'s `node` at the first hit is not
    described by the DWARF (see below); `&g_counter` printing its pointee over
    `--ipc` is `disable_addresses`, and its address is in the RDI (`ModuleOff`,
    `global_addr_cast` checks it through `cast(uint64)&g_counter`); the
    registers after a watchpoint stop are fresh (see below); `eval signo` at
    the handler's first line read 30 in 9 of 9 runs here (the mechanism on
    hand for a 0 is the first-read caveat: `signo` lives at `sp+12` of the
    signal frame, a page the frontend has never read).

Policy, not bugs (the suite expects these):

* A continue after an exception stop does not deliver the exception to the
  debuggee: the engine resumes with `ignore_previous_exception`, which the
  demon answers with KERN_SUCCESS (a hardware exception re-executes its
  instruction and faults again: another stop at the same pc) or `PT_THUPDATE`
  signal 0 (a soft signal is dropped: `raise(SIGINT)` returns, exit code 0), as
  upstream's Linux demon (`PTRACE_CONT` with signal 0) and Win32 demon
  (`DBG_CONTINUE`) do. A handler runs, or an unhandled crash terminates the
  process, when the exception's "Break on ..." setting is off.
* A step into a function stops at its opening brace (the prologue's line), as
  upstream does; the first statement is one step over away.
* A `__thread` variable reads 0 until the thread has touched one of the
  image's thread-locals (dyld allocates the block lazily, so
  `dmn_thread_get_module_tls_vaddr` finds none before).

Open, recorded with evidence by the suite (detach, fork, exec into a binary
of our own, watchpoint attribution and other threads' call stacks have been
fixed since by the demon and unwinder work and are not repeated here):

* Fixed by the merged dyld-notification work (recorded before the merge):
  only the executable and dyld were listed, dylib breakpoints never resolved
  and the TLS getter stopped in code without a module; after the merge
  `breakpoint_dylib`, `modules` (load, unload, reload) and `D3`'s module list
  pass, `U5` lands in `libdyld.dylib` with `main` as frame 1 and steps out to
  the TLS line. Likewise fixed by the merged frame-selection and unwinder
  work: `F7` (the step into executes an instruction), `D7` (the stop in the
  new thread selects it), the call stacks of other threads (`U3`,
  `threads_freeze`, `D5` finds the spinner) and the fiber stack
  (`range -> entryPoint -> next`).
* `eval write` now resolves (the system libraries' RDIs come from their
  symbol tables, see Demon), but `D3`'s `write` breakpoint still does not
  hit: `basic.c`'s `printf` reaches `write$NOCANCEL` (`__write_nocancel`),
  not `write`, on this macOS. A program calling `write()` itself stops there
  (`tests/dyld_module_events.sh`).
* exec (`exec`, `D6`): `execv`/`execl` fail inside the debuggee (exit 5, the
  `perror` path; see the scenario's NOTE for errno).
* Watchpoints (`D8`): the condition is not honored (the stop comes at the
  first write, `D8_condition_honored_*`): in `d_ctrl_thread__run`'s breakpoint
  bookkeeping the conditions are gathered from the user traps whose `vaddr`
  equals the event's `instruction_pointer`; a watchpoint's trap carries the
  data address (which the demon reports as the event's `address`, with the
  trap id as `user_data`), so no condition is found and `event->user_data !=
  0` stops unconditionally (matching `trap->flags != 0 && trap->vaddr ==
  event->address` as well would gather it; run loop, not changed here). The
  values the `watchpoint` scenario saw lagging the write by one stop were the
  scripting caveat above (an `eval` answered from a page whose refresh had not
  landed), not the stops: with `ev` waiting for two agreeing answers the
  scenario passes. (The missing attribution to the user breakpoint,
  `user_data`, is fixed by the demon work; `tests/demon/watchpoint.sh`.)
* Step out of a signal handler runs to the process' end
  (`signal_handler/handler_step_out`); a step over `longjmp` as well
  (`longjmp`; gdb/lldb stop after `setjmp`).
* Calls through stubs: a step into on a line calling `strlen` (through
  `__stubs`) or a weak template instantiation (`twice<int>` in `cpp_O0` goes
  through a stub too) lands on the stub's first instruction; there the
  unwinder reports the caller's caller as frame 1 (`main` instead of
  `libsystem_call`: the frame-pointer chain is used although nothing was
  pushed yet), so the following step out returns one frame too far, and a step
  over the stub's indirect jump runs to the process' end (`stepping_O0`,
  `stepping_cpp`). The same frame-1 loss shows at any function's first
  instruction after an instruction step into a call (`stepping_O0/inst_into_call_stack`).
* Step over a tail call (`b tail_target` at -O2) ends in the callee's first
  line instead of after the call in the caller (`stepping_O2`).
* After a watchpoint stop, the first `eval` of a value reads the previous
  stop's copy of its page (`i`, `sum`, `g_watched` which cannot coexist in
  program order; `watchpoint` NOTEs, polled three times per stop while
  investigating): the registers are not stale (the engine bumps its register
  generation at the end of every run and re-reads the thread from the demon
  after its internal step: `pc` reads the instruction after the store), the
  frontend's memory cache hands out a page it already has while the refresh
  is in flight (`ac_artifact_from_key` without `AC_Flag_WaitForFresh`), and the
  second poll has the fresh page; `ev` waits for two agreeing answers. The
  same ip for consecutive stops is the store instruction itself (the loop
  writes `g_watched` from one `str`): `state` reports the stop event's
  address, the accessing instruction, while `pc` is one past it.
* A stop in a new thread created after another thread exited evaluates in the
  main thread's context (`D7`: `idx` reads 0, the call stack lacks
  `worker_body`), although the thread list is right.
* `eval` of locals held in registers at -O2: `F4a`'s `r` (in x0) read 0:
  fixed, see "Evaluation fixes" below (DWARF register 0 mapped to the nil
  register). `F6`'s `node` "could not be found" at the first hit of the
  breakpoint on the `visit` line is right: that hit is in the copy of `visit`
  inlined into `main` (depth 1), where clang describes only `sum`
  (`DW_OP_consts 10, DW_OP_stack_value`) and emits no entry for `node`; the
  later hits (`visit` inlined into the block, `node` in x22) read it
  (`{20, 2, ...}`). In an outer frame a local in an argument register reads
  the inner frame's register (no availability in the unwind, see "Status and
  open issues").
* In a caller's frame `rec->name` read `""` while `(*rec).name` read `"rec"`
  (`eval_O0` NOTE): no asymmetry between the two paths -- the first expression
  to touch `g_record`'s page answers from nothing when the asynchronous page
  read has not landed (whichever form it is: evaluating `(*rec).name` first
  gives `""` for it and `"rec"` for `rec->name`), the scripting caveat above;
  the scenario evaluates `(*rec).name` first and checks `rec->name` after it.
* `dylib_host_O0` ends with exit code 0 and without its final `printf` output
  under the debugger (natively: "host done 8", exit 8) after the second
  `dlclose` (`modules/exit_code`).
* After stepping out of the TLS getter onto the middle of the `t_tls_counter
  += 1` line, a step over stops on that same line again (`U5_next_line`): the
  line's remaining rows after the call count as a new line, so two steps are
  needed to reach the next line.
* Flaky: `eval signo` at the first line of a signal handler read 0 in some
  runs and 30 in others (`signal_handler/handler_signo`); not reproduced since
  (9 of 9 runs read 30). `on_sigusr1` is frameless (`DW_AT_frame_base
  DW_OP_reg31`, `signo` at `DW_OP_fbreg +12`, i.e. `sp+12`), so the frame base
  comes from the thread's `sp` at the stop, which is right; the mechanism on
  hand for a 0 is the first-read caveat above (the signal frame is a stack
  page the frontend has never read, and `ev`'s two polls can both come before
  the page lands).
* `F7`: with a disassembly tab focused, a step into at the first instruction
  of an inlined call only selects the next inline frame (pc unchanged).
* `F1`/`F2`: a step over at inline depth 1 from `next`'s last line stops at the
  helper's instruction LLVM scheduled into that line (shown as the helper's
  call line at depth 1), not at main's second call; identical on 20 runs.
  `F1b`: a step over `next`'s first line ends on the same line.
* `F4b`: the breakpoint on `inl.h:3` first stops in the second inlined
  instance (call site `inl_main.c:4`). `F6`: a step into on a line whose first
  call goes into libSystem (`qsort_b`) stops there, without line info.

## Known gaps / follow-ups

* Rendering uses one shared `NSOpenGLContext` that is re-targeted with
  `setView:` when the frame switches windows. With a single window this is
  fine; with several windows it may flicker because the window-server surface
  follows the context. The fix is one context per window (created with
  `shareContext:`) and a per-window vertex array object.
* Text input reads `NSEvent` characters directly; dead keys and CJK input
  methods do not compose (would need `interpretKeyEvents:` /
  `NSTextInputClient`).
* Caps Lock produces one press when it turns on and one release when it turns
  off, rather than a press/release pair per keystroke.
* `wm_dim_from_monitor` reports a screen's own backing pixels while window
  rects use the primary screen's scale; only cosmetic on mixed-DPI setups.
* Executables with only a debug map (`N_OSO` stabs) and no dSYM are not
  linked by the converter itself; it runs `dsymutil` when available.
* Shared-cache images: on this macOS every image's `LC_SYMTAB` keeps its local
  symbols, so the symbol-only files name them. A production cache (macOS 12-15
  shipped `dyld_shared_cache_<arch>.symbols` next to the cache) strips the
  locals into that file's `dyld_cache_local_symbols_info` chunk and would need
  a reader (locate the file by the running cache's `symbolFileUUID`, match the
  image's entry by its offset from the cache base, append the nlists to the
  symbol-only file); not written, since no such cache was available to test
  against. Code with no symbol at all stays unnamed either way. The images
  cache is swept (30 days / 1 GB); radbin's `dsyms/` and the engine's `rdis/`
  caches are not.
* Hardware execute breakpoints are implemented as software `brk` traps;
  watchpoints use `DBGWCR`/`DBGWVR` (up to 16, 8-byte aligned ranges).
