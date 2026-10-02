# PHETAMINE — design

> Scope: build and test against accounts/builds you own or are authorized to
> test. Anti-detection is out of scope by design — see `README.md` § Scope.

## 1 · What "internal" means, and what it deletes

The previous draft was an external controller with the process boundary moved:
it still shipped Lua into the game that phoned home over `127.0.0.1:9753`. Every
artifact of that design existed only because an external process could not touch
the VM. When you *are* the VM, they all die:

| Bridge artifact | Why it existed | Native internal |
|---|---|---|
| `http_server.h` on `127.0.0.1:9753` | only channel into the game | **deleted** — named pipe from the UI |
| Lua init script | bootstrap the bridge | **deleted** — init is C++ |
| `CoreGui.PHETAMINE` folder | "init succeeded" signal | **deleted** — IPC `READY` |
| `GET /poll` every 100 ms | deliver scripts | **deleted** — direct `luau_load` + run |
| `POST /ls` → module anchor | only way to run new code | **deleted** — compile in C++, load, run |
| `POST /req` proxy | Lua could not do net | **deleted** — `request()` is a C closure over WinHTTP |
| `unc_payload.h` | UNC surface written in Lua | **deleted** — native C closures (`src/native_api/`) |
| Module hijack (jest/test/spec) | get code into the engine loader | **deleted** — never write a ModuleScript |
| RSB1 + BLAKE3 + ZSTD container | satisfy the module loader | **deleted** — raw Luau bytecode via `luau_load` |
| PlayerListManager `+0x8` spoof + ESC race | trick the engine into `require` | **deleted** — replaced by the shellcode-resident loader (§4) |
| `SetBytecode` / `RestoreAllModules` | clean up the above | **deleted** — nothing is ever overwritten |

The same deletion is what makes a real UNC surface possible: `getidentity`,
`checkcaller`, `iscclosure`/`islclosure`, `newcclosure`, `clonefunction`,
`hookfunction`, `getcallingscript`, `getsenv`, `getreg`, `debug.info` all need
the VM, and here the VM is ours.

## 2 · Architecture

```
┌────────────────────────────┐
│   PHETAMINEUI.exe (C# WPF) │   external, dumb terminal
│  - find PID / inject       │   (reads PHETAMINEAPI.dll, hands it to the stub)
│  - NamedPipeClientStream   │
│  - editor, script list,    │
│    auto-exec, log pane     │
└─────────────┬──────────────┘
              │  \\.\pipe\PHETAMINE_<pid>
              │  READY · EXECUTE · LOG · INFO · STOP · CANARY · UNLOAD
              ▼
┌────────────────────────────────────────────────────────────────────────────┐
│ RobloxPlayerBeta.exe                                                       │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │ PIC stub (shellcode-resident loader, no CRT, PEB-walk imports)       │  │
│  │   VirtualAlloc ▸ map sections ▸ relocs ▸ imports ▸ TLS ▸ entry       │  │
│  └───────────────────────────┬──────────────────────────────────────────┘  │
│  ┌───────────────────────────▼──────────────────────────────────────────┐  │
│  │ PHETAMINEAPI.dll (manually mapped, entry = core::Initialize)         │  │
│  │  core/        init pipeline · DataModel · ScriptContext · watchdog    │  │
│  │  memory/      offsets resolver + probes · instance walker (rblx::)    │  │
│  │  lua/         state acquisition · luau_* binding · identity · genv    │  │
│  │  scheduler/   main-thread rendezvous + drain (the ONLY VM touchpoint) │  │
│  │  executor/    luau_compile → luau_load → setidentity → resume         │  │
│  │  native_api/  the UNC surface, one C closure per function             │  │
│  │  net/         WinHTTP client called from `request()` on a worker      │  │
│  │  ipc/         named pipe server                                       │  │
│  └───────────────────────────┬──────────────────────────────────────────┘  │
│                              ▼  C closures registered into genv            │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │ Roblox Luau VM  ·  genv (__index → renv)  ·  one thread per script    │  │
│  │   getgenv() loadstring() request() setidentity() hookfunction() …    │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
└────────────────────────────────────────────────────────────────────────────┘
```

## 3 · Init pipeline (`core::Initialize`, worker thread, never `DllMain`)

1. `base = GetModuleHandleW(nullptr)`; sanity-check the PE (`MZ`, `PE\0\0`, x64).
2. `offsets::Load()` — parse the embedded table, require
   `Roblox Version` to match the running build's version string. Mismatch is not
   fatal (AOB fallback), but every unresolved entry disables its dependent
   feature (§[`OFFSETS.md`](OFFSETS.md)).
3. `walker::GetDataModel()` — `FakeDataModel::Pointer` → `RealDataModel`;
   validate `ClassDescriptor` name is `"DataModel"`; fallback = TaskScheduler job
   walk (`JobStart`/`JobEnd`) looking for the `RenderJob`/`DataModel` entry.
   No DataModel → `ERROR:DataModel`, unload, the UI tries the next PID.
4. `walker::AcquireScriptContext()` → `ScriptContext` instance; then
   `lua::AcquireState()`:
   a. candidate scanning: walk the ScriptContext's pointer slots for a pointer
      that passes the state probe (§3.1) — this beats a hardcoded
      `ScriptContext+0xNNN` that moves every build;
   b. AOB scan fallback;
   c. offset fallback from the table.
5. `lua::ResolveApi()` — bind `luau_compile`, `luau_load`, `lua_resume`,
   `lua_pcall`, `lua_newthread`, `lua_pushcclosurek`, `lua_ref`/`lua_unref`,
   `lua_setfield`, plus the fork helpers `lua_setreadonly` / `lua_setsafeenv` /
   `lua_resetthread` **and** the client's `free` (see §5, correction 2).
6. `lua::BuildEnvironment()` — create `genv` (`__index` → `renv`), pin with
   `lua_ref`, create `_G` and `shared`, register every capability that passed
   its probe.
7. `scheduler::Start()` — install the drain rendezvous (§6).
8. `ipc::Start()` — pipe `\\.\pipe\PHETAMINE_<pid>`, then `READY` + `INFO` + the
   canary bitmap (`CANARY`).
9. `core::Watchdog()` starts. Every ~500 ms: DataModel valid, `JobId`/`PlaceId`
   unchanged, scheduler heartbeat fresh. On change → `RebindSession()`.

### 3.1 State probe (fail closed)

`ProbeState(L)` must pass **before** `L` is trusted:

- registry/globals table readable, `game` present and an instance,
- `print` and `task` present and callable-shaped,
- `lua_ref` round-trip on the globals table succeeds,
- after `BuildEnvironment()`, the canary (§7) must pass at identity 8.

Any failure → disable dependent capabilities, log the stage, and continue in a
degraded-but-safe state. Never write through an unprobed pointer.

## 4 · Loader: shellcode-resident mapping (ADR-1)

The old path tricked the engine into running code (PlayerListManager `+0x8`
spoof + simulated ESC-key race to hit `require`). That is both fragile (three
engine behaviours must line up inside a frame) and invasive. Replacement:

1. UI reads `PHETAMINEAPI.dll` from disk into a buffer (the payload).
2. UI allocates the target's staging block, writes: `[ParamBlock][stub bytes][dll bytes]`.
3. UI starts the **PIC stub** at the staging block (`CreateRemoteThread`; an APC
   path is implemented behind `--apc` but off by default — see DECISIONS ADR-1
   for the safety analysis that keeps it off).
4. The stub is freestanding C (`src/inject/stub/stub.c`): no CRT, no imports
   table, kernel32/ntdll resolved by a hashed PEB walk. It maps the payload
   image (sections, relocations, imports, TLS), then calls the DLL entry with
   the param block, and reports a status code back through the block.
5. The DLL's `DllMain` only does `DisableThreadLibraryCalls` +
   `CreateThread(core::Initialize)`.

Result: no engine module is read, hijacked, or written; no `require` race; the
loader itself is data + shellcode, so the UI needs no loader DLL on the target.

## 5 · Spec corrections (verified against the Luau fork in this ecosystem)

These are differences between the draft spec and the VM we actually bind to.
Each one is a crash or a wrong-behaviour if coded as originally written.

1. **`lua_resume` is Luau's, not Lua 5.4's.**
   `LUA_API int lua_resume(lua_State* L, lua_State* from, int narg);`
   — three arguments, **no `nresults` out-param**; results are on the thread
   stack. Yields come back as `LUA_YIELD` (and `LUA_BREAK` for debug breaks).
2. **`luau_compile` allocates with the client's allocator.**
   `LUACODE_API char* luau_compile(const char* source, size_t size,
   lua_CompileOptions* options, size_t* outsize);` returns a `malloc`'d buffer.
   Our DLL is `/MT` (static CRT) — calling our `free()` on the client's pointer
   is a heap mismatch. We resolve the client's `free` alongside the VM entry
   points and release through it (`executor::Bytecode` RAII).
3. **`lua_resetthread` returns `void`** in this fork (Lua 5.4 returns an int).
   Stopping a script therefore means: call it, then *assume nothing* — retire
   the thread ref whether or not it cleaned up.
4. **`lua_pushcclosure` is a macro** over
   `lua_pushcclosurek(L, fn, debugname, nup, cont)`. Register through
   `pushcclosurek` with a real debug name so `debug.info` output looks native.
5. **`lua_ref`/`lua_unref` and `lua_setreadonly`/`lua_setsafeenv` exist** in the
   fork, so no pure-Lua fallbacks are needed for env pinning or renv protection
   — but they are still probed before use (they are fork-optional, and a
   different client build may drop them).
6. **The public offset dump has no VM entries.** `offsets.imtheo.lol` publishes
   instance/data fields (`TaskScheduler`, `FakeDataModel`, `DataModel`,
   `Instance`, …) and a `ScriptContext` namespace that currently contains only
   `RequireBypass`. So: the walker is table-driven; the VM layer is
   probe-driven. Two independent public sources already disagree on
   `Instance::Name`/`Children` for their respective builds — evidence for
   "never hardcode, always probe" (§[`OFFSETS.md`](OFFSETS.md)).

## 6 · Scheduler: the only VM touchpoint

Roblox Luau is single-threaded and owned by the client's main thread. Resuming a
thread from our own worker while the engine is mid-frame is the classic source of
"crashes after 10 minutes".

- `Enqueue(Job)` is a lock-free SPSC ring: IPC and worker threads only push.
- `Drain()` runs **on the client main thread** and is non-blocking,
  exception-free, and allocation-light. Every dereference is SEH-guarded.
- Rendezvous strategies, in probe order:
  1. **ScriptContext task queue** (preferred) — enqueue into the same queue
     `task.defer` uses, so the engine itself calls us at a legal point.
  2. **Frame site** — an AOB-scanned once-per-frame site in the task scheduler
     that we call through.
  3. **APC rendezvous** — `NtQueueApcThread` on the render thread; only for
     shellcode-level work (never VM calls) because an APC can land inside an
     allocator or lock. Off by default.
  4. **None** — capability `scheduler` = false, `EXECUTE` returns
     `ERROR:Scheduler` instead of pretending.
- Job kinds: `RunScript(bytecode, envRef, identity)`, `CallClosure(ref, args)`,
  `RebindSession`, `RestoreInstances`, `SetIdentity`, `Shutdown`.
- **Yielding scripts** (`wait`, `task.wait`, `coroutine.yield`): on
  `LUA_YIELD`, read the requested delay from the yielded values, park
  `{thread, resumeAt}` in `YieldingThreads`, and resume on a later drain under a
  per-frame budget (~1.5 ms) with a resume cap, so 500 `task.spawn`s cannot
  hitch a frame.
- `task.spawn`/`task.defer` inside scripts route through the ScriptContext queue
  we already resolved, keeping engine semantics intact.

## 7 · Boot canary

After init, and again after **every rebind** (teleport), a small internal chunk
runs at identity 8 and exercises one real thing per capability: `getgenv`
round-trip, `setidentity` round-trip, `newcclosure` + `hookfunction` +
`checkcaller`, `getrawmetatable`, `gethui`, `readfile`/`writefile`, `request`
(skipped offline). The pass/fail bitmap goes out as `CANARY` and drives both the
UI capability panel and `PHETAMINE.capability(name)`. A failed canary entry
**deregisters** the function (the caller gets `"unsupported"`), it never leaves
a broken function installed.

## 8 · Watchdog, teleports, shutdown

- Teleports do not unload us: the watchdog sees the DataModel/JobId invalidate,
  calls `RebindSession()` — re-acquire DataModel/ScriptContext/state, rebuild
  `genv`, re-verify identity, replay `queue_on_teleport` + `workspace/autoexec/`
  — and re-runs the canary. No re-injection.
- `core::Shutdown()` order: stop accepting IPC → drain/discard scheduler jobs →
  retire our Lua threads (`lua_resetthread`, else abandon the ref) → unregister
  closures → drop registry refs → stop threads → unmap. Nothing live is left
  behind.
- Fatal conditions (DataModel null after 3 rebind attempts, stalled scheduler
  heartbeat, more than N VEH-caught faults from our ranges) → log the stage,
  shutdown, unload. The worst outcome is a half-initialized module in a live
  process, so **fail closed, always unload, never idle.**

## 9 · Capability reporting

`PHETAMINE.capability(name)` → bool, and the same bitmap in the UI panel. Scripts
branch on absent features instead of crashing into them. A capability is `true`
only if (a) its resolution probe passed and (b) its canary entry passed on the
current session. Disabled functions are **not registered at all** where that is
cleaner than a stub — a plausible-looking fake scores the same on UNC as an
absent one and costs debugging time forever.
