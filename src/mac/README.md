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
  signal is the initial stop.
* Waiting: a kqueue with `EVFILT_MACHPORT` (exception port set),
  `EVFILT_PROC` (`NOTE_EXIT`) and `EVFILT_USER` (halt requests).
* Stops: on the first exception message the whole task is suspended with
  `task_suspend`, stragglers are drained, `task_threads` is diffed against the
  known thread list (thread create/exit events), and registers are read with
  `thread_get_state` (`ARM_THREAD_STATE64`, `ARM_NEON_STATE64`).
* Resume: pending exception messages are replied to (`PT_THUPDATE` chooses
  the signal to deliver for soft signals; `KERN_FAILURE` escalates a hardware
  exception to its BSD signal when the engine did not ask to ignore it),
  frozen threads are `thread_suspend`ed, then `task_resume`.
* Breakpoints: `brk #0`; the PC points at the trap instruction when reported
  (no rollback). Single stepping: `MDSCR_EL1.SS` via `ARM_DEBUG_STATE64`.
  Watchpoints: `DBGWCR/DBGWVR` via `ARM_DEBUG_STATE64`; since arm64 reports
  them before the access, the demon steps past the access internally.
* Modules: a permanent `brk` on dyld's `_dyld_debugger_notification`
  (`x0` = mode, `x1` = count, `x2` = `dyld_image_info[]`) reports loads and
  unloads; the demon steps over its own trap internally. Mode 3
  (`dyld_image_dyld_moved`, dyld handing over to its copy in the shared cache)
  re-targets the trap to the new dyld. As a safety net, every stop compares
  `dyld_all_image_infos.infoArrayCount` with the last seen count and diffs the
  image list when it changed (or when the trap could not be placed; the
  `notification` pointer is used as a fallback once dyld has rebased, with
  pointer-authentication bits stripped). Module info is parsed from the
  in-memory Mach-O header (`__unwind_info`, `__eh_frame`, UUID, entry point,
  `__thread_vars`).
* Exception codes reported to the engine: BSD signal numbers, or
  `0x10000 + EXC_*` for Mach exception types (see `mac/mac.mdesk`).
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
  scans ahead for a return through the remaining stack pops; in those states
  the return address is `lr`, the caller's `fp` is still in `fp`, and only
  the entry stack pointer is computed from the code. In DWARF mode the CFI row
  is evaluated with `fp` pointed back at the intact frame record, so that the
  callee-saved registers are restored from their slots as usual. Verified on a
  Linux host against the prologues and epilogues of a Zig-built arm64 binary.

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

## Status and open issues (2026-10-25, evening)

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
* Still open in IPC: the *first* reply of a fresh instance is delivered to
  the instance itself (its listener receives a packet of the reply's size
  right after the reply is sent), so the first `--ipc` client waits out its
  10 s timeout; every later command is answered within ~40 ms. The connection
  lookup in `sock_async_tick` (`mac_socket.c`) is where to look.
* Driving the debugger from a script: `launch_and_step_into` sent over `--ipc`
  has no target in its registers and launches nothing; send `step_into` with
  no process instead. Fire one throwaway command in the background right
  after the port accepts connections, to absorb the first-reply stall. `exit` over IPC does not quit an instance with a live
  process (kill it by pid). A `step_into` at the first instruction of an
  inlined call only selects the next inline frame, without a run. Each IPC
  `state` renders a frame, so poll at 100ms or slower. The harness used
  (`h.sh`: launch, breakpoint, run, step, printing every stop with its inline
  frames) runs a breakpoint-and-steps scenario in about a second.

Open:

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
  lookup is available on that thread). A UI-level "step out, then resume the
  step into" was tried and removed: at the first instruction of a frameless
  function without known unwind info the unwinder falls back to the frame
  pointer chain and skips the callee's frame, so the step out returned one
  frame too far (`range` -> `entryPoint`). That unwinder weakness is itself
  worth fixing (prefer `lr` when nothing was pushed yet); shared-cache
  modules may also lack unwind info because dyld's notification hook is not
  found here ("dynamic library load events will be synthesized at stops").
* `step_out` from an inlined function steps out of the concrete function; it
  could use the selected inline site's ranges (`step_out_scope` already works
  from scope ranges).
* Optimized code attributes scheduled instructions to the function's
  declaration line (`start.zig:641` fragments between other lines); stepping
  visits them, as other debuggers do.
* A headless driver (the demon from a console program, the way `torture`
  does) would still be useful; the `--ipc` loop above is the next best thing.

## Converter conformance corpus (2026-10-25, night)

`src/mac/tests/macho_corpus/` holds a Mach-O test corpus and a checker for the
DWARF → RDI converter; `RESULTS.md` there is the table of the last run (with a
hand-maintained findings section at the end).

* `build_corpus.sh [-o <dir>]` builds the matrix into a directory outside the
  repository (default `$TMPDIR/raddbg_macho_corpus`): C at `-O0`, `-O2`,
  `-O3 -fomit-frame-pointer`, `-flto`, `-gdwarf-3`, `-gdwarf-4`, with relative
  source paths, and with `DW_AT_call_file`/`DW_AT_call_line` stripped from the
  inline sites (assembly rewrite); C++ (templates, virtual calls, exceptions,
  lambdas) at `-O0`, `-O2`, `-flto`; a dylib with an executable linking it and
  one `dlopen()`ing it; a static library (`N_OSO` into the archive); threads
  with `_Thread_local`; large frames / VLAs / `alloca`; deep recursion; signal
  handlers; an arm64+x86_64 universal binary; the Zig generator in Debug,
  ReleaseSafe and ReleaseFast (`zig build --prefix ... --cache-dir ...`, the
  project is not touched); Rust (`split-debuginfo=unpacked`) and Swift. Every
  entry comes twice: with a `.dSYM` next to it, and as the bare executable with
  only its debug map, so that `radbin` runs `dsymutil` itself.
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
  names, nesting); inline sites have function types; source line maps agree
  with the tables; file-scope globals and thread-locals exist with their
  addresses. 70 binaries take about 25 s; the largest (the Zig Debug
  generator, 2.7 MB of DWARF) converts in 0.1 s.

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

Open: Swift functions directly under `DW_TAG_module` get no function type (56
of 84 procedures in `swiftprog-O`; struct methods do), so their inline sites
have none either; the checker does not look at locals' locations or types;
code that the line program attributes to an inlinee's line outside the site's
ranges keeps that line in the enclosing table (the row-merging heuristic; a
Rust `dealloc` example is in `RESULTS.md`).

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
* Hardware execute breakpoints are implemented as software `brk` traps;
  watchpoints use `DBGWCR`/`DBGWVR` (up to 16, 8-byte aligned ranges).
