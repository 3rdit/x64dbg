# Step Over (F8) for the ElfBug Linux debugger

**Status:** Approved, not yet implemented
**Date:** 2026-08-08
**Component:** `src/cross/ElfBug`, `src/cross/debugger`
**Target branch:** feature branch off `development`, PR to the `3rdit/x64dbg` fork

## 1. Motivation

`MainWindow::onStepOver()` (`src/cross/debugger/gui/MainWindow.cpp:292`) is a stub:

```cpp
void MainWindow::onStepOver() const
{
    onLogMessage("[x64dbg] Step-over not implemented, using step-into");
    onStepInto();
}
```

There is a toolbar button, an F8 shortcut and a "Step Over (F8)" tooltip, all of which
single-step instead. Stepping over a `call` is the most-used operation in a debugger, so
today F8 dives into every function, including PLT stubs and the dynamic linker resolver.

The engine side is a declared-but-empty stub: `Process::StepOver` at
`src/cross/ElfBug/ElfBug/process/Breakpoint.cpp:97` is `(void)cbStep;`, carrying the comment
`// TODO: implement with Zydis disassembly` at `Process.h:48`.

This spec covers **Step Over only**. The underlying primitive is built as a general
mechanism so that Step Out and Run-to-Cursor become small follow-up PRs that add a call
site each, but neither is exposed here.

## 2. Goals and non-goals

**Goals**

- F8 steps over `call`, `rep`-prefixed string operations, and `pushf`.
- Correct behaviour under recursion (do not stop in the wrong frame).
- Correct behaviour with other threads running.
- No stale `0xCC` can ever be left in the tracee.
- The engine remains self-contained: `ElfBugStepOver` works for any API consumer, not
  only the Qt GUI.

**Non-goals**

- Step Out, Run-to-Cursor, tracing, conditional breakpoints.
- All-stop mode (see Known Limitations).
- Hardware breakpoints, memory breakpoints, attach/detach — other stubs in the engine.
- Symbolication (ELF/DWARF). Unrelated work stream.

## 3. Prior art: how Windows x64dbg does it

Read from `src/third_party/TitanEngine/TitanEngine/TitanEngine.Debugger.Control.cpp:71`:

```cpp
__declspec(dllexport) void TITCALL StepOver(LPVOID StepCallBack)
{
    ULONG_PTR ueCurrentPosition = GetContextData(UE_CIP);
    unsigned char instr[16];
    MemoryReadSafe(dbgProcessInformation.hProcess, (void*)ueCurrentPosition, instr, sizeof(instr), 0);
    char* DisassembledString = (char*)StaticDisassembleEx(ueCurrentPosition, (LPVOID)instr);
    if(strstr(DisassembledString, "CALL") || strstr(DisassembledString, "REP") || strstr(DisassembledString, "PUSHF"))
    {
        ueCurrentPosition += StaticLengthDisassemble((void*)instr);
        SetBPX(ueCurrentPosition, UE_BREAKPOINT_TYPE_INT3 + UE_SINGLESHOOT, StepCallBack);
    }
    else
        StepInto(StepCallBack);
}
```

**Adopted:**

- The primitive is a breakpoint at `rip + instructionLength`, not a loop of single-steps.
- Three instruction classes are stepped over, not one: `CALL`, `REP`, `PUSHF`.
- `PUSHF` is included as an anti-anti-debug measure. Single-stepping sets `EFLAGS.TF`, so a
  tracee executing `pushf` would push a flags word with `TF` set and could detect the
  debugger. This applies identically on Linux: `PTRACE_SINGLESTEP` sets `TF`, and while the
  kernel masks that bit from the *tracer*'s `PTRACE_GETREGS` view (the `TIF_FORCED_TF`
  logic), it does not intercept the tracee's own `pushf` — the CPU pushes the real flags.
- Step state is per-thread (`engineStepThreads` keyed by thread id) so a pending step on one
  thread does not block another.

**Deliberately not adopted:**

- Classification by `strstr` on disassembly *text*. It is fragile — an operand or symbol
  containing "CALL" would match. x64dbg's own newer code abandoned this in favour of
  `Zydis::IsCall()` / `IsBranchType(...)` (`src/dbg/_exports.cpp:815`). We use Zydis
  predicates.
- The absence of a stack-depth guard. TitanEngine's `StepOver` has none, so a call site
  re-entered recursively stops in the wrong frame. x64dbg only applies the guard in
  run-to-return (`src/dbg/debugger.cpp:1426`,
  `if(gRtrPreviousCSP <= csp)  //"Run until return" should break only if RSP is bigger than or equal to current value`).
  We apply an equivalent guard to step-over itself.

The instruction classes are ISA-level and transfer unchanged from PE to ELF. The one
ELF-specific consideration is the PLT: on a dynamically linked ELF the first `call` to each
external symbol traps into `_dl_runtime_resolve`, so step-over is *more* valuable on Linux
than on Windows. The return-address breakpoint handles it with no special casing.

## 4. Design decisions

| # | Decision | Rationale |
|---|---|---|
| 1 | Step Over only; primitive built general | Smallest reviewable diff that still gets the architecture right. TitanEngine builds `StepOut` on `StepOver`, confirming the layering. |
| 2 | Decode inside the engine; hoist `zydis_wrapper` | Keeps ElfBug a complete debugger usable headlessly and testable without Qt. |
| 3 | Step over `CALL` + `REP` + `PUSHF`, via Zydis predicates | Behavioural parity with x64dbg, without the `strstr` fragility. |
| 4 | Step-over state object in `Debugger`; never double-plant | Avoids refcounting the physical INT3, which would mean refactoring already-hardened breakpoint code. |
| 5 | Third resume mode in `Debugger`, classification in `Process` | Mirrors the proven `mStepPending` pattern; keeps the testable part pure. |

### 4.1 Rejected alternatives

- **Hand-rolled length decoder in ElfBug.** Zero dependencies, but x86-64 length decoding
  (prefixes, REX, VEX/EVEX, ModRM/SIB, immediates) is a correctness minefield, and a wrong
  length silently plants an INT3 mid-instruction and corrupts the tracee.
- **Decode in `DbgAdapter` using `QZydis`.** No build changes, but stepping policy would
  leak into the GUI, the engine's headless tests could not cover step-over, and any future
  non-Qt consumer would have to reimplement the policy.
- **`BreakpointType::Internal` enum member.** `softwareBreakpointReferences` is keyed by
  address alone with one saved `oldbytes`, so two logical breakpoints at one address would
  independently save and restore, and the restore could write back a `0xCC` as the
  "original" byte. Only workable with refcounting, which is a larger change.
- **Refcount the physical INT3.** The general answer, and probably right eventually, but it
  refactors code hardened by `44126f77` and `65983232` for a feature that does not need it.
- **A `StepController` class now.** Correct destination once Step Out and Run-to-Cursor
  exist; premature with one consumer. Extract it in the Step Out PR.

## 5. Architecture

Five units. Only two hold state.

### 5.1 `Process::ClassifyStepOver` — pure policy (new)

```cpp
enum class StepOverKind { None, Call, Rep, Pushf };   // types/Global.h, beside StepCallback

// Pure: no process, no ptrace. Unit-testable with a byte buffer.
StepOverKind ClassifyStepOver(const uint8* bytes, size_t n, ptr rip, ptr& nextAddr);
```

A thin `Process::` wrapper performs `MemRead(rip, buf, 16)` and forwards. Classification uses
Zydis predicates (`IsCall`, rep-prefix presence, `PUSHF`/`PUSHFQ` mnemonic). Returning the
*kind* rather than a bool is required by §6.2 — the three kinds are not treated identically.

This replaces the dead `Process::StepOver(const StepCallback&)` stub; no orphan API remains.

### 5.2 `Debugger::StepOver()` — cross-thread request

Line-for-line twin of `Debugger::StepInto()` (`core/Debugger.cpp:176`): lock `mPauseMutex`,
set `mStepOverPending`, clear `mPaused`, `mPauseCv.notify_one()`. Declared in the public
block of `Debugger.h` after `StepInto()`.

### 5.3 `Debugger::mStepOver` — debug-thread-owned state

```cpp
struct StepOverRequest
{
    bool  active     = false;
    ptr   target     = 0;      // rip + instructionLength
    pid_t tid        = 0;      // only this thread may complete it
    ptr   rspFloor   = 0;      // frame-identity guard value
    bool  frameGuard = false;  // whether rspFloor applies at all
    bool  planted    = false;  // did we write the INT3, or ride an existing one
};
```

Private nested struct in `Debugger.h`; nothing else needs it. `mStepOverPending` is an
`std::atomic<bool>` beside `mStepPending` (`Debugger.h:72`); `mStepOver` is a plain member in
the same group, which already carries the comment *"Tracer-thread only; caller threads must
not write"* (`Debugger.h:69`) — exactly this ownership rule, so no locking is needed on it.

### 5.4 `pauseAndResume()` — third resume branch

Currently a two-way choice between `PTRACE_SINGLESTEP` (when `mStepPending`) and
`PTRACE_CONT`. Gains a third branch that classifies at RIP and either plants-and-continues
or falls through to the existing single-step path.

### 5.5 `handleSigtrap()` — completion, filtering, cancellation

Detects our target being hit, decides complete / ignore-and-resume / cancel.

### 5.6 Supporting additions

- `Process::HasBreakpoint(ptr addr) const` — lookup over `softwareBreakpointReferences`.
- `Debugger::stepPastBreakpointByte(pid_t pid, ptr addr) -> bool` — see §5.7.
- `ElfBugStepOver(ElfBugDebugger* dbg)` in `api/elfbug_api.h`, mirroring `ElfBugStepInto`
  including its null-and-inactive guards.
- `DbgAdapter::StepOver()` in `src/cross/debugger/core/DbgAdapter.{h,cpp}`.
- `MainWindow::onStepOver()` calls it; the "not implemented" log line is deleted.

### 5.7 One targeted refactor of existing code

`core/Debugger.Loop.Signal.cpp:176-231` contains the remove-byte → single-step → reinstate
dance inline, roughly 55 lines nested deep inside the breakpoint branch. §6.3 needs to invoke
that same dance from a second place. Extract it as:

```cpp
bool Debugger::stepPastBreakpointByte(pid_t pid, ptr addr);
```

and have both call sites use it. This is the only change to existing hardened code, and it is
forced by the feature rather than opportunistic.

### 5.8 Layer summary

```
MainWindow::onStepOver()    -> real call; "not implemented" log deleted
DbgAdapter::StepOver()      -> ElfBugStepOver(mDebugger)
ElfBugStepOver(dbg)         -> Debugger::StepOver()                  [thread-safe]
Debugger::StepOver()        -> mStepOverPending = true; cv.notify_one()
pauseAndResume()            -> classify; plant if absent; CONT | SINGLESTEP
handleSigtrap()             -> complete | ignore-and-resume | cancel
```

## 6. Data flow and lifecycle

### 6.1 Happy path (a `call`)

```
UI thread     Debugger::StepOver()   -> mStepOverPending = true; cv.notify
debug thread  pauseAndResume() wakes
                kind = ClassifyStepOver(rip) -> Call, nextAddr = rip + 5
                HasBreakpoint(target)? no -> SetBreakpoint(target); planted = true
                mStepOver = {active, target, tid = pid, rspFloor = Gsp(),
                             frameGuard = true, planted}
                PTRACE_CONT
              ... callee runs ...
              SIGTRAP at target + 1
handleSigtrap   bpAddr = Gip() - 1 == mStepOver.target
                tid matches and RSP >= rspFloor -> complete
                if planted: DeleteBreakpoint(target)
                mStepOver = {}
                beginPause(); cbStep()
```

Completion reports `cbStep()`, not `cbBreakpoint()` — including the ride-along case where a
user breakpoint also sits on the target. The user pressed F8; reporting "breakpoint hit"
would be surprising. The user's breakpoint is left armed and untouched for future hits.

### 6.2 The frame guard is not uniform

The obvious implementation records RSP and accepts a hit when `RSP >= rspFloor`. That is
correct for `call` and **wrong** for `pushf`:

| Kind | RSP at target, relative to `rspFloor` | Guard |
|---|---|---|
| `Call` | `==` (call pushes 8, `ret` pops 8) | **apply** `RSP >= rspFloor` |
| `Rep` | `==` (string ops do not touch RSP) | not needed |
| `Pushf` | **`== rspFloor - 8`** (it pushes) | **must not apply** |

With a uniform guard, `pushf` fails the check on its own completion, is silently resumed, and
the step-over never finishes: the INT3 stays planted and F8 appears to hang until some other
stop cancels it. Hence `frameGuard` is a separate field, set only for `StepOverKind::Call`.

`Rep` and `Pushf` need no guard for an independent reason: they complete in a single
instruction, so the thread cannot recurse into the same site before trapping.

For `Call`, `>=` rather than `>` is correct: at the call site RSP is `S`; the call pushes to
`S-8`; `ret` pops back to `S`. Equality is the normal completion case. A deeper recursive hit
at the same address has `RSP < S`.

### 6.3 Foreign-thread and wrong-frame filtering

Other threads run free during the `PTRACE_CONT`, and the INT3 is process-wide. A trap at the
target is ignored — resumed transparently via `stepPastBreakpointByte(pid, addr)`, with no
callback, no pause, and `mStepOver` left armed — when either:

- `pid != mStepOver.tid` (a different thread reached the address), or
- `frameGuard && Gsp() < mStepOver.rspFloor` (same thread, deeper frame).

Because the breakpoint must survive these ignored hits, it is planted **non-singleshot** and
its lifetime is managed explicitly. A singleshot breakpoint would be consumed by the first
foreign hit and the step-over could never complete.

### 6.4 Cancellation — one rule

Any stop reported to the user, other than the completion itself, cancels the step-over:
delete the planted INT3 (only if `planted`), clear `mStepOver`, then report the stop
normally. This covers a user breakpoint inside the callee, a signal or exception, an explicit
Pause, thread exit, and `ElfBugStop`. Process exit clears the state without touching memory,
which is already gone.

Invariant bought: **a planted INT3 never outlives the stop that follows it.**

### 6.5 Ordering that makes the collision rule sound

The API layer's pending-breakpoint queue is drained by `processPendingBreakpoints()` inside
`cbStep`, `cbPaused`, `cbBreakpoint` and `cbPauseTick`
(`api/elfbug_api.cpp:325-352`), all of which run *before* `pauseAndResume()` consumes the
step request. So when `HasBreakpoint(target)` is evaluated, queued user breakpoints have
already been applied and `planted` cannot be computed against a stale view.

Note also that the API layer keeps its own user-facing `breakpointAddrs` set
(`api/elfbug_api.cpp:36`), separate from `Process::breakpoints`. `ElfBugIsBreakpointEffective`
reports from that set, so an engine-level internal breakpoint is invisible to the GUI's
breakpoint markers for free.

## 7. Error handling

Every failure degrades to a plain single-step. Step-over is an optimisation over
single-stepping, so any inability to do the clever thing falls back to the thing that always
works.

| Failure | Response |
|---|---|
| `MemRead(rip, buf, 16)` fails | single-step; no error callback (RIP may sit near an unmapped edge) |
| Zydis fails to decode | single-step; no error callback |
| `kind == None` | single-step; normal path, not an error |
| `SetBreakpoint(target)` fails | single-step **and** `cbInternalError` — genuinely unexpected |
| `PTRACE_CONT` fails after planting | cancel, remove INT3, existing error path |
| `stepPastBreakpointByte` fails on an ignored hit | cancel, remove INT3, report error; never leave the byte patched |

**Re-entrancy.** A second request cannot arrive mid-flight, since requests are consumed only
at a stop and `mStepOver.active` is cleared before any stop is reported. Defensively,
consuming a request while `active` is set cancels the stale one first rather than leaking its
breakpoint.

## 8. Build changes

`zydis_wrapper` is currently pulled in as a subdirectory *inside* `widgets`
(`src/cross/widgets/CMakeLists.txt:3-6`), so ElfBug — which links only `Threads::Threads` —
cannot reach it.

1. Hoist the `add_subdirectory(...zydis_wrapper...)` call out of
   `src/cross/widgets/CMakeLists.txt` (hand-maintained; it has no `cmake.toml`) up to the
   `cross` level.
2. Add `zydis_wrapper` to `[target.ElfBug].link-libraries` in `src/cross/cmake.toml`.
3. Regenerate `src/cross/CMakeLists.txt` via cmkr — it is generated and marked DO NOT EDIT.

`ElfBug/tests` links `ElfBug`, so it picks Zydis up transitively.

**Formatting.** `.github/format/AStyleHelper.py` pins `astyle==3.6.9` via a PEP 723 header;
the machine's `/usr/bin/astyle` is 3.6.17 and would format differently. Run
`uv run .github/format/AStyleHelper.py` so the pinned wheel is used. Options are
`style=allman, convert-tabs, align-pointer=type, align-reference=middle, indent=spaces,
indent-namespaces, indent-col1-comments, unpad-paren, keep-one-line-blocks,
close-templates`.

## 9. Testing

### 9.1 Classifier unit tests (no tracee)

`ClassifyStepOver` is a pure function over a byte buffer, so it is tested directly with
hand-fed encodings: `call rel32`, `call r/m64`, `rep movsb`, `rep stosq`, `pushfq`,
REX-prefixed forms, and negatives (`mov`, `jmp`, `ret`, `syscall`, `nop`). This covers the
decode matrix far more densely than live tests can, and runs instantly.

### 9.2 Live tests

Byte-exact instruction placement cannot be guaranteed from C++ even at `-O0`, so one new
fixture uses file-scope `asm()` inside a `.cpp` — no CMake language changes, and it matches
the existing `sources = ["targets/*.cpp"]` pattern in `ElfBug/tests/cmake.toml`. It exports a
global label per instruction of interest:

```
so_call_site:     call so_callee
so_recurse_site:  call so_recurse    # frame-guard test
so_rep_site:      rep movsb
so_pushf_site:    pushfq
so_plain_site:    nop                # fallback path
```

`ResolveRuntimeAddress(path, pid, "so_call_site")` (`tests/SymbolHelper.h:60`) resolves each
label at runtime via `nm` plus load bias. Fixtures build `-g -O0 -pie` through the existing
`elfbug_fixture` template. Cases are tagged `[stepover]`, alongside the existing `[step]`:

1. **Call** — step over `so_call_site`; assert a `Step` event with
   `instructionPointer == so_call_site + 5`, and that the callee's side effect occurred, so
   it really ran rather than being skipped.
2. **Frame guard / recursion** — step over the recursive call site at depth 1; assert we land
   in our own frame, not a deeper one. Fails if the guard is dropped.
3. **`PUSHF` does not leak TF** — step over `so_pushf_site`, read the pushed qword at RSP and
   assert bit 8 (`TF`) is clear. Proves the anti-debug property the case exists for, and it
   is the test that catches a uniform frame guard, since that makes this case hang to
   timeout.
4. **REP** — step over `rep movsb` with a large count; assert exactly one `Step` event
   rather than N.
5. **Fallback** — step over `so_plain_site`; assert behaviour identical to `StepInto`.
6. **Breakpoint inside callee cancels** — user breakpoint in `so_callee`; step over the call;
   assert a `Breakpoint` event rather than `Step`, then assert the original byte is intact at
   the return address. This is the stale-INT3 regression test, reusing the byte-comparison
   pattern from `tests/tests.cpp:202` ("Software breakpoint patches and restores instruction
   byte").
7. **Ride-along** — user breakpoint already on the return address; step over; assert a `Step`
   event, and that the user's breakpoint still fires on a later `Continue`, proving we did
   not delete what we did not plant.

Tests 3 and 6 are the two that would actually catch a broken implementation; the rest are
coverage.

### 9.3 Manual verification

Build and launch the GUI, open a dynamically linked ELF, and confirm F8 over a `call` to a
library function lands on the next source instruction instead of entering
`_dl_runtime_resolve`.

## 10. Known limitations

**Free-running-thread race.** `stepPastBreakpointByte` briefly restores the original byte in
order to single-step past it. With other threads running, the stepping thread could cross the
target inside that window and miss the trap, leaving the step-over armed until something
cancels it. This window already exists for every breakpoint resume in the engine today; it is
the reason all-stop mode is a TODO at `core/Debugger.Loop.Signal.cpp:37`. Fixing it means
implementing all-stop, which is its own PR. Documented in a code comment, not covered by a
test — reproducing it needs scheduling control the harness does not have, and any such test
would be flaky.

**`longjmp` and unwinding past the target.** If the callee unwinds past our frame, the target
is never reached and the step-over stays armed until the next stop cancels it. Acceptable:
the invariant in §6.4 still guarantees the INT3 is removed.

## 11. Files touched

**New**

- `src/cross/ElfBug/ElfBug/process/StepOver.cpp` — `ClassifyStepOver` and the `Process`
  wrapper.
- `src/cross/ElfBug/tests/targets/step_over_targets.cpp` — asm fixture.

**Modified**

- `ElfBug/ElfBug/types/Global.h` — `StepOverKind`.
- `ElfBug/ElfBug/process/Process.h` — `HasBreakpoint`, `ClassifyStepOver`; remove the
  `StepOver` stub declaration.
- `ElfBug/ElfBug/process/Breakpoint.cpp` — remove the `StepOver` stub body.
- `ElfBug/ElfBug/core/Debugger.h` — `StepOver()`, `mStepOverPending`, `StepOverRequest`,
  `mStepOver`, `stepPastBreakpointByte`.
- `ElfBug/ElfBug/core/Debugger.cpp` — `StepOver()`.
- `ElfBug/ElfBug/core/Debugger.Loop.cpp` — third resume branch in `pauseAndResume`.
- `ElfBug/ElfBug/core/Debugger.Loop.Signal.cpp` — completion, filtering, cancellation; extract
  `stepPastBreakpointByte`.
- `ElfBug/ElfBug/api/elfbug_api.h` / `.cpp` — `ElfBugStepOver`.
- `ElfBug/tests/cmake.toml` — new fixture target.
- `ElfBug/tests/tests.cpp` — `[stepover]` cases.
- `src/cross/cmake.toml` + regenerated `src/cross/CMakeLists.txt` — Zydis link.
- `src/cross/widgets/CMakeLists.txt` — hoist `zydis_wrapper`.
- `src/cross/debugger/core/DbgAdapter.h` / `.cpp` — `StepOver()`.
- `src/cross/debugger/gui/MainWindow.cpp` — real `onStepOver`.

## 12. Follow-up work unlocked

- **Step Out** — same primitive, target is the return address. Reliable discovery needs CFI
  (chapters 15-16 of the knowledge base); an RBP walk is wrong under
  `-fomit-frame-pointer`. Good moment to extract `StepController`.
- **Run to Cursor** — same primitive, target from the GUI selection. No decoding needed.
- **All-stop mode** — closes the §10 race and unblocks reliable multithreaded stepping.
