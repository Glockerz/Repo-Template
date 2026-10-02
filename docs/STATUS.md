# Status

Two lists, and the difference between them is the whole point of this file:

- **What this sandbox has verified** — checks that ran here and passed, with the
  command that ran them.
- **What only a Windows host can verify** — everything that requires the client,
  a compiler, or both. Nothing in this list may be described as working; at best
  it is *written, reviewed against the fork's headers, and statically checked*.

## 1 · Verified in this sandbox (no toolchain, no client)

| Check | Command | Result |
|---|---|---|
| IPC opcode tables agree (C++ ↔ C#) | `node tools/verify.mjs` | 10/10 checks pass |
| every registry row documented + implemented | `node tools/verify.mjs` | 78 rows, 78 symbols, 78 documented |
| offset keys the code asks for exist | `node tools/verify.mjs` | 24 referenced, 65 in table |
| no deleted bridge artifact in `src/` or `ui/` | `node tools/verify.mjs` | 15 needles, 0 hits |
| loader-blob generator (synthetic PEs) | `node tools/gen/shellcode_blob.mjs --self-test` | passes |
| module-image contract (relocations, export) | `node tools/gen/check_module.mjs --self-test` | 5/5 cases |
| offsets pipeline, offline | `node tools/offsets/fetch_offsets.mjs --from …/observed_02c37bc51a384b8f.json` | 65 keys, validates |
| cross-file name resolution in `src/` | `node tools/gen/crossref.mjs --strict` | 56 files, no unresolved names |
| all of the above in one command | `npm test` | passes |

`crossref.mjs` is the honest answer to "you cannot compile here": it parses every
header and every source file and fails on a call to a function that no header ever
declares, or on `api.<member>` where `lua::Api` (src/lua/state.h) has no such
member. It does **not** type-check, does not resolve overloads, and cannot see a
signature mismatch.

## 2 · Written but NOT verified (needs a Windows host)

Everything in `src/`, in one list, because listing it per-module would imply a
confidence that does not exist yet:

1. **The module does not compile yet in this sandbox** — there is no C++
   toolchain here (`docs/CONTEXT.md`). It was written against the fork's public
   headers (`VM/include/lua.h`, transcribed in `docs/PHETAMINE.md §5`) and
   checked by `crossref.mjs`, but no compiler has seen it.
2. **Signatures taken from the fork's header are not proven at runtime:**
   `lua_resume(L, from, narg)` (a fork signature, not stock Lua 5.4),
   `lua_resetthread` returning void, `lua_pushcclosurek`, `lua_ref`/`lua_unref`,
   `luau_compile`'s option struct, and `lua_Debug`'s field order.
3. **Every structural offset in `src/lua/layout.h` is `kUnset`** and must be
   filled per build (`docs/OFFSETS.md §5`): state field offsets, namecall
   `TString*`, closure function/proto, instance→userdata, userdata→instance.
   Until then `checkcaller`/`identity` still work (thread set + extra space) but
   `hookfunction` is off by design.
4. **Assumed-but-unproven VM behaviours** that the code is written around:
   * `lua_getmetatable` ignoring `__metatable` (used by `getrawmetatable`);
   * `lua_getfenv`'s stack effect for functions/threads;
   * `lua_setuserdatametatable` taking the metatable from the top of the stack
     (only relevant if the tag-level metatable path is ever used);
   * `lua_next`'s iteration order over the registry (only the merged-into-genv
     path depends on it, and that path is bounded to 512 entries).
5. **The rendezvous** (Heartbeat via `RunService.Heartbeat:Connect` from the
   bootstrap) is written but not run. The one-shot `WH_GETMESSAGE` bootstrap in
   `src/inject/loader.cpp` is the other assumption: it needs the client to own a
   window and pump messages on the thread that owns it.
6. **The loader** (`src/inject/stub/stub.c`) is freestanding code that no
   compiler here has parsed. Its CMake recipe (`/NOENTRY`, `/MERGE:.rdata=.text`,
   `PREFIX ""`) is unverified, as is the claim that the linked stub has zero
   relocations — `check_module.mjs` verifies that on a real build.
7. **The UI** is not built here (no .NET SDK on this host): its Debug-only error
   target, the P/Invoke signatures, and the WinForms layout are all unverified.
8. **Offset values are from the public dump for `version-02c37bc51a384b8f`** and
   the VM keys are not published there at all — they resolve by export → scan →
   probe, and any that fail disable exactly one feature (`docs/OFFSETS.md §4`).

## 3 · Named gaps (by design, not accidents)

`docs/UNC_COVERAGE.md` is the authoritative list. The short version:

* `cloneref`, `firetouchinterest`, `fireclickdetector`, `fireproximityprompt`,
  `getscriptclosure` **refuse by name** — the primitive they need is unresolved.
* `getinstances`, `getnilinstances`, `getgc`, `getconnections` return empty and
  say so once; they are marked `partial`.
* `decompile`, `saveinstance`, `Drawing` are out of scope for this pass.

## 4 · Windows test order (the only way to promote anything out of §2)

1. `node tools/selftest.mjs && node tools/verify.mjs` — must be green before a
   build is even attempted.
2. `cmake --build build --config Release` — `check_module.mjs` must accept the
   DLL, and the blob generator must accept the stub.
3. Load the module into a **test client** you own with a debugger attached, and
   confirm the log reaches `stage Ready` and the UI receives `Canary`.
4. Inject into a real client and confirm the stage list in the UI log runs
   `Api → DataModel → ScriptContext → LuaState → Environment → Registry →
   Scheduler → Canary → Ready` with no `ERROR:<stage>` line.
5. `print(getgenv() == getgenv())` must be `true`, and
   `PHETAMINE.capability("scheduler")` must be `true`.
6. Run the UNC harness and record the **measured** score in
   `docs/UNC_COVERAGE.md` — not before.
7. Negative tests, one at a time: kill the Heartbeat connection and confirm
   `scheduler` turns false and EXECUTE answers `ERROR:Scheduler`; break one
   offset on purpose and confirm that capability reports `off` instead of
   crashing.
8. Test the teleport path: `queue_on_teleport("print('replayed')")`, join another
   place, confirm the rebind replays it and the canary is re-posted.
9. Unload: `OP_UNLOAD` from the UI, confirm every stage stops, the pipe closes,
   and the process (for a manual map) has no mapped image left — the trampoline
   in `inject::SelfUnmapIfManual` is the mechanism to watch.
