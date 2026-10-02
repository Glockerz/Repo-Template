# PHETAMINE — native internal executor

**Native internal** means: a UI process injects a manually-mapped DLL into the
Roblox client, that DLL owns the Luau VM (`lua_State`, `luau_compile`,
`luau_load`, `lua_resume`), and scripts run **in-process on the Roblox main
thread** through C closures. There is no HTTP server in the game process, no Lua
bootstrap script, no polling worker, no module hijack, no bytecode container.

This repo is also the template repo. The [Agent Operating Protocol](#agent-operating-protocol)
below is unchanged and still applies to every session.

---

## Scope & boundaries (read this first)

- **Target only accounts and builds you own or are authorized to test.** Run it
  in a private place, on a private client build. Nothing here is a service.
- **What is deliberately *not* in this repo:** anti-cheat / anti-detection
  engineering — no module-list or header scrubbing to hide from scans, no
  PEB/ETW patching, no telemetry spoofing, no per-antichreat bypasses, no
  "detection-wave" tuning. The stability machinery that *is* here (probes,
  fail-closed resolution, SEH/VEH guards, clean unload) exists so a half
  initialized module cannot crash the client you are testing on. That is a
  robustness property, not a stealth property.
- Expect offsets and signatures to break on every client update. §
  [`docs/OFFSETS.md`](docs/OFFSETS.md) is the contract that makes a stale build
  *fail closed* instead of crashing.

## Documents

| Doc | What it is |
|---|---|
| [`docs/PHETAMINE.md`](docs/PHETAMINE.md) | Design: what native internal means, the pipeline, spec corrections |
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | File-by-file map of `src/`, `ui/`, `tools/` |
| [`docs/OFFSETS.md`](docs/OFFSETS.md) | Resolution order, theo's-dumper schema, probe rules |
| [`docs/UNC_COVERAGE.md`](docs/UNC_COVERAGE.md) | UNC tiers with per-function status |
| [`docs/STATUS.md`](docs/STATUS.md) | What is written / verified / unverifiable here |
| [`docs/DECISIONS.md`](docs/DECISIONS.md) | ADRs (shellcode-resident loader, in-process compiler, …) |

## Layout

```
README.md                      ← you are here (protocol + project identity)
ToDo.md                        ← roadmap + Session Memory
docs/                          ← doctrine, context, PHETAMINE design docs
src/                           ← PHETAMINE.dll (C++20, MSVC/clang-cl x64)
  core/ lua/ scheduler/ executor/ native_api/ net/ memory/ ipc/ inject/ common/
ui/PHETAMINEUI/                ← WinForms front end (net8.0-windows, Debug only, pipe client only)
CMakeLists.txt                 ← module + freestanding loader stub + post-build image check
tools/                         ← host-side tooling (Node, dependency-free)
agent/                         ← the passthrough + memory + verification
```

## Build (Windows host — see `docs/STATUS.md` for what is not built here)

```powershell
# 0 · the host-side checks run anywhere (Node ≥ 22.18): npm test
npm test

# 1 · the module + the loader stub (Windows, MSVC or clang-cl, x64)
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release        # PHETAMINE.dll, PHETAMINEStub.dll
                                            # → build/PHETAMINE.stub.bin (generated)

# 2 · the UI (Debug only; a Release build FAILS on purpose)
cd ui/PHETAMINEUI; dotnet build -c Debug
```

`src/memory/offsets_table.inc` is generated from `offsets_embedded.json`, and the
loader blob is generated from the linked stub — both as CMake custom commands, and
both refuse to emit something that fails validation. A post-build step
(`tools/gen/check_module.mjs`) reads the linked DLL and fails the build if it has
base relocations or is missing `PhetamineEntry`, because those two properties are
what make the shellcode map work at all (ADR-1).

The **UI must never be built Release** — see
[`ui/PHETAMINEUI/README.md`](ui/PHETAMINEUI/README.md).

---

## Agent Operating Protocol

**This repository is the template for every future repo an agent interacts with.**
It is self-describing: doctrine, context, plans, memory, and a capability-verified
single-command context loader all live in the repo, so any fresh agent session can
cold-start, remember, and prove itself before doing work.

### Quick start (for agents)

```bash
node agent/context.ts          # ONE typed passthrough, run before EVERY response
```

That single command replaces the legacy ~30-command bash ritual
(`git status`, `git log`, `git diff`, `ls -R`, `cat` of every doc, tailing logs,
counting tokens, checking the environment, …). It parses the full context, reads
short-term memory, and emits a problem-solving board. See `docs/CONTEXT.md` for
all flags (`--note`, `--quiet`, `--json`, `--probe`).

### How each clause is implemented

| Clause | Implementation |
|---|---|
| Template for every future repo | This repo: doctrine (`docs/DOCTRINE.md`), context (`docs/CONTEXT.md`), plans + memory (`ToDo.md`) |
| `ToDo.md` → Session Memory section | `ToDo.md` has `## Session Memory`, mirrored from the full log `agent/memory/session.jsonl` (logs everything) |
| 30 bash commands → one TypeScript passthrough | `agent/context.ts` (zero-dependency, Node type stripping), run before every response |
| Parse full context / short-term memory / problem-solve | The passthrough brief: git + tree + docs, short-term window, problem-solving board |
| Read doctrine/context files | The brief dumps `README.md`, `docs/DOCTRINE.md`, `docs/CONTEXT.md`, `ToDo.md`, capabilities |
| Verify image-sight, thinking, frontier, context tolerance | Live probes; evidence in `agent/verification/CAPABILITIES.md` |

### Session lifecycle

1. `node agent/context.ts --note "<why this session started>"` — read the brief.
2. Do the requested work on the session branch only (`arena/<session>-repo-template`).
3. `node agent/context.ts --note "<what was done>"` before ending the turn.
