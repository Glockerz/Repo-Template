# ToDo

## Tasks

Add your tasks below — one per line.
`- [ ]` = open (the agent picks these up as objectives), `- [x]` = done.

### PHETAMINE — native internal executor

Done in this session (all statically checked, none compiled — `docs/STATUS.md` §2):

- [x] design docs + ADRs (`docs/PHETAMINE.md`, `docs/DECISIONS.md` ADR-1…10)
- [x] offset pipeline: 65 keys from the public dump, probe/disable law (`docs/OFFSETS.md`)
- [x] the module source tree: `core`, `lua`, `scheduler`, `executor`, `ipc`, `native_api`, `net`, `inject`, `common`
- [x] the loader: freestanding `stub.c` + blob generator + `check_module.mjs`
- [x] the UI (Debug-only) + the opcode table shared with `protocol.h`
- [x] `npm test` → verify 10/10, crossref clean, every tool self-test green

Open, in the order a Windows host should do them:

- [ ] Build it: `cmake -S . -B build -A x64 && cmake --build build --config Release`. The
      post-build `check_module.mjs` must accept the DLL; the blob generator must accept the stub.
- [ ] Compile fixes: nothing here has seen a compiler. Expect signature and include work,
      especially in `lua/state.cpp` (bindings) and `inject/stub/stub.c` (freestanding build).
- [ ] `lua/layout.h`: fill the per-build structural offsets (`docs/OFFSETS.md §5`) and confirm the
      identity round-trip, the namecall `TString*`, and closure layout probes on the live client.
- [ ] Rendezvous: confirm the Heartbeat connection is installed and that `scheduler` reports true;
      then run the negative test in `docs/STATUS.md §4.7`.
- [ ] UNC harness against a live client; record the **measured** score in `docs/UNC_COVERAGE.md`.
- [ ] Soak: teleport ×2, rejoin (rebind + `queue_on_teleport` replay), then `UNLOAD` and confirm the
      manual-mapped image is gone and no thread of ours survives.
- [ ] Refresh offsets for the next client version (`npm run offsets:fetch`, then `npm test`).

<!-- example, copy it or delete it:
- [ ] my first task
-->

## Session Memory

Logged automatically by `node agent/context.ts` — the 12 most recent entries
are mirrored here; the full unpruned log lives in `agent/memory/session.jsonl`.

<!-- session-log:start -->
- 2026-10-02T02:03:07.895Z · arena/01a0fa58-repo-template@1f6995b · session start: PHETAMINE native-internal executor request received; running Repo-Template protocol + first-session capability probes before scope decision (dirty:0)
- 2026-10-02T02:26:35.181Z · arena/01a0fa58-repo-template@1f6995b · PHETAMINE tree complete (uncompiled): core/scheduler/executor/ipc/native_api/net/inject written; loader stub + check_module + crossref + UI; npm test green (verify 10/10); next: Windows build. (dirty:5)
- 2026-10-02T02:28:11.256Z · arena/01a0fa58-repo-template@ad246a1 · Pre-compile audit pass: missing includes, namespace placement (sched::jobs::RebindOnMainThread), env::SetSession definitions, rendezvous bootstrap env index fixed; lua/threads.cpp added to CMakeLists; npm test green. (dirty:0)
<!-- session-log:end -->
