# Architecture decisions

Each entry records a decision that constrains the code. When a decision changes,
it is edited here and the code is edited with it — a stale ADR is worse than no
ADR, because the next reader will trust it.

---

## ADR-1 — the loader is shellcode-resident; the PlayerListManager detour is gone

**Context.** The first draft of PHETAMINE did what most public executors did at
the time: spoofed `PlayerListManager + 0x8` so a `require` of a real module would
hand back our payload, then raced the escape key so the resulting error screen
closed in the same frame. That design has three independent failure modes before
it does any useful work — the module it needs must be *not yet loaded* (it
usually is by the time the user executes anything), the identity it borrows must
be high enough for `require` (it is not, for anonymous identities), and the race
must win (it wins on a warmed-up client and loses on a cold one).

**Decision.** The UI injects a freestanding, position-independent x64 loader
(`src/inject/stub/stub.c`) into the client process. The stub resolves the ntdll
routines it needs from the PEB, maps `PHETAMINE.dll` — allocating at the
preferred base, mapping sections, applying relocations, resolving imports,
setting section protections — parses the mapped image's export directory for
`PhetamineEntry`, and calls it with a `ParamBlock` (`src/inject/param.h`). The
DLL then starts its own init thread from that entry (there is no module table
entry, so nothing in the process can `FreeLibrary` it) and unmaps itself on
shutdown through a stub page *outside* the image.

**Consequences.**
* No engine structure is written during load and no timing race exists: the only
  thing that changes in the target is memory the injector allocated.
* The image must have **zero base relocations** for the preferred-base map to
  succeed deterministically; `tools/gen/shellcode_blob.mjs` enforces that the
  blob carries none, and the blob applies the DLL's relocations rather than
  assuming they are absent.
* Unloading is our own problem. `core::Shutdown()` → `inject::SelfUnmapIfManual()`
  builds a trampoline outside the image that terminates the caller, unmaps the
  image, and then terminates itself from ntdll. The 4 KiB trampoline page leaks
  by design (it cannot free the page it is executing from) and that cost is
  accepted, once, at shutdown.
* Manual mapping is still visible to an integrity scanner; this is an engineering
  trade, not a stealth claim (see ADR-7).

---

## ADR-2 — compilation happens in-process, with the client's own Luau

**Context.** A worker that compiles and then ships bytecode over IPC removes the
compiler from the client, but it introduces a bytecode container that has to be
versioned against the client's Luau, and a failed compile on the worker side
cannot be attributed to the client's parser.

**Decision.** `luau_compile` and `luau_load` are resolved from the client image
(export → cache → signature → bundled Luau) and called in-process. `exec::Compile`
allocates with the client's `luau_compile`, and the resulting buffer is released
through the client's `free` — resolved as the offset key `crt.free` — never
through our CRT. If `crt.free` cannot be resolved, the buffer is intentionally
leaked with a loud error: freeing client memory with the wrong heap corrupts the
process, and a leak of one compile buffer is survivable.

**Consequences.** Compile errors are the client's compile errors, verbatim. The
IPC protocol never carries bytecode (see `docs/UNC_COVERAGE.md`, `loadstring`).

---

## ADR-3 — IPC is one named pipe, one client, no loopback socket

**Context.** The draft design ran an HTTP server on `127.0.0.1:9753` inside the
client, and the UI polled `GET /poll`. That adds a firewall prompt on some
machines, ties the tool to a port number, and means the UI must handle a client
restart with a reconnect storm.

**Decision.** `\\.\pipe\PHETAMINE_<pid>` (`src/ipc/pipe_server.cpp`), a single
`u32 length | u8 opcode | payload` frame format, one accepting thread, one
reader, one writer. The PID in the name lets the UI attach to an
already-injected process after its own restart. Opcode values are checked against
the C# client by `tools/verify.mjs` (check 4), so the two sides cannot drift.

**Consequences.** The UI is a dumb pipe client: it finds a PID, injects, connects,
and sends EXECUTE. It never builds a payload, never parses Lua, and never needs to
know an offset. Frame size is capped at 8 MiB per side.

---

## ADR-4 — no `.text` hooks; `hookfunction` swaps closures instead

**Context.** Patching five bytes at the top of an engine function works, until
the client updates and the five bytes are a different function's. It also makes
our own code unrecoverable if it crashes inside the trampoline.

**Decision.** Nothing in `src/` writes to the client's executable sections. The
only writes we perform outside our own memory are:
* the fps cap (`setfpscap`), through the offset table, SEH-guarded, and marked
  failed on a fault;
* the identity slot of threads we own;
* section protections for our *own* manually mapped image.

`hookfunction` (when the `hooks` capability is on) replaces a closure's function
pointer — a data write with a verified layout offset — and refuses when the
layout is unknown. `sigs::kSigs` is empty by design and every row is tagged with
the capability it gates; a signature is added only when it can be verified by a
behaviour probe.

**Consequences.** A signature resolve that fails disables exactly one feature
(ADR-6). We never need to reason about original-byte restoration on unload, and a
failed unload leaves at most our allocations behind.

---

## ADR-5 — rendezvous order: Heartbeat first, then the fallbacks, then nothing

**Context.** The drain must run on the client's main thread, because Roblox Luau
is single-threaded and the engine's VM state is owned by that thread. Candidate
mechanisms have very different footprints and failure modes.

**Decision.** The order is:

1. **Heartbeat** (implemented, default). During the one-shot main-thread
   bootstrap a C closure is connected to `RunService.Heartbeat` (and
   `PostSimulation`) from inside the VM. The engine then calls us once per frame,
   at a point it already considers legal. No signatures, no patches, no offsets.
2. **TaskQueue / FrameSite** (reserved, not installable yet). Both need a
   per-build address in the cache under `sched.taskqueue.insert` /
   `sched.rendezvous.site`, *and* a call from a context that may enter the VM —
   which the init worker is not. `rendezvous::Install()` therefore reports a
   cached address and leaves it alone; the strategy has to be wired in the
   bootstrap alongside Heartbeat. They stay ordered after Heartbeat because they
   require writing a callback pointer into engine structures.
3. **APC** (implemented, **off by default**). `QueueUserAPC` on a client thread
   can only run during an alertable wait, which is exactly what makes it *safe*
   for VM work — but most client threads never enter one, so it is a probe, not a
   plan. Enabling it is a per-session decision, and a session where it is enabled
   still reports the honest result of `sched::rendezvous::TryArmApcProbe()`.
4. **None** — init fails with `ERROR:Scheduler` and EXECUTE says so. A scheduler
   that is "probably fine" is worse than a scheduler that is absent.

The bootstrap itself is delivered by `inject::RequestMainThreadBootstrap`, a
one-shot `WH_GETMESSAGE` hook on the thread that owns the game window: the OS
calls our procedure while that thread pumps messages, which is a moment when the
VM is idle. If no window exists and no hook can be installed, bootstrap fails and
init fails closed.

**Consequences.** The whole scheduler can be tested without a single signature.
`docs/STATUS.md` §4 lists the Windows test order, including the negative test:
kill the heartbeat (unrecoverably) and confirm `PHETAMINE.capability("scheduler")`
turns false and EXECUTE reports an error instead of silently dropping jobs.

---

## ADR-6 — every offset and signature fails closed, per feature

**Context.** Dumper output lags client updates by days. A hardcoded `0x68` that
moved is a wild write into engine memory, which is how "the executor crashed my
client" happens.

**Decision.** `off::Get(key)` returns `0` for any key that is absent, unresolvable
or probed-failed, and callers treat `0` as "feature off". Load order is version
table → AOB scan → probe → **disable**. A failed probe calls
`off::MarkProbed(key, false)`, which both evicts any cached address and turns the
entry into `Failed` for the rest of the session; dependents that consult
`off::StatusOf` then refuse to run. No default constants exist anywhere in
`src/memory/offsets.cpp` or `offsets_table.inc`.

**Consequences.** A bad build degrades to "some features are off", announced via
`PHETAMINE.capability()` and the IPC `Canary` frame. It never degrades to a crash
or to a plausible-looking lie.

---

## ADR-7 — no anti-detection engineering

**Context.** The most common request after "make it work" is "make it undetected".
That is a different project with a different risk model, and it is not this one.

**Decision.** No anti-cheat or anti-detection bypass engineering: no PPID spoofing,
no ETW/WMIC patching, no thread hiding, no handle stripping, no certificate games,
no obfuscated strings, no encrypted payload images, no dynamic API hashing for the
purpose of hiding imports. We do not attempt to stop a protected client from
observing us. Where we use a manual map or Windows APIs that injectors commonly
use, it is because those APIs *work reliably*, not because they hide.

**Consequences.** The repository's threat model is stated in `README.md` § Scope.
This is what makes the shellcode loader's "deliberate omissions" list in
`stub.c`—TLS callbacks, header erasure, exception rewriting—small: they are
detection-relevant and correctness-neutral here.

---

## ADR-8 — one `lua_State` per script, and an ownership table

**Context.** `checkcaller()` has to know whether the current thread is ours, and
a script that yields must be resumable without resuming the engine's own thread.

**Decision.** Each EXECUTE compiles to bytecode, creates a fresh thread
(`lua_newthread`), loads the bytecode into it with the genv, applies the identity
when the identity capability is on, and resumes it. Scripts that yield are parked
in a table with a resume deadline (≤64 resumes per frame, ≤1.5 ms drain budget)
and resumed later from the same main-thread drain. `lua::threads` keeps a
bloom-filtered set of our threads (SRW-locked); `checkcaller()` and
`getcallingscript()` answer from it, and `lua::CurrentScriptThread()` is set for
the duration of each job so the answer is precise rather than approximate.

**Consequences.** `stop-scripts` retires every thread we know; a retired thread is
reset when the API allows it and abandoned otherwise (`try_reset` reports what it
could do). Abandoning is deliberate: a corrupted thread is worse than a leaked one.

---

## ADR-9 — UNC is implemented as C closures, not a Lua payload

**Context.** The classic design ships a `.lua` bootstrap that rebuilds the
library functions on top of a handful of primitives. That means shipping a Luan
payload, a prelude, and a fragile "which primitive exists" negotiation.

**Decision.** Every UNC function in `src/native_api/registry.def` is a C closure
registered into genv. The registry is the single source of truth: `tools/verify.mjs`
checks it against `docs/UNC_COVERAGE.md` (no missing rows, no duplicates), the
capability enum in `api.h`, and the C# client's opcode table. Dotted Lua names
(`debug.info`, `crypt.encrypt`) are registered into a *shadow* subtable whose
`__index` is the engine's own table, so we never mutate engine tables.

**Consequences.** Adding a function is one `.def` row plus one `l_<symbol>` in the
matching TU. A function that cannot be implemented honestly is not registered at
all; its row is absent and its name is listed as unsupported in
`docs/UNC_COVERAGE.md`.

---

## ADR-10 — the capability bitmap is a first-class API

**Context.** "It silently didn't work" is the single most expensive failure mode
for a user, and it is indistinguishable from "you called it wrong".

**Decision.** Capabilities (`Cap::Env`, `Cap::Instances`, …) are probe-driven at
boot and in the post-injection canary, exposed twice: `PHETAMINE.capability(name)`
in Lua, and the IPC `Capabilities`/`Canary` frames for the UI. Off means off: an
off capability's functions are not registered (the registry counts them as
`skipped`), and a call that cannot be served raises a Lua error naming the
capability rather than returning a plausible stub.

**Consequences.** The UI's capability panel is a view of the same bitmap the VM
sees, so the two can never disagree. `docs/UNC_COVERAGE.md` states, per function,
whether it is implemented, partial, or unsupported on this build — and a partial
function says so in its own log line.
