# Repo Context

Machine-oriented context for agents. Humans see `README.md`; agents read this
plus the passthrough brief.

This checkout is the **Repo-Template** and, in the same branch, the **PHETAMINE**
native-internal executor. Template rules still govern how an agent works here;
`docs/PHETAMINE.md` and friends describe the product.

## Layout

| Path                              | Purpose                                                        |
|-----------------------------------|----------------------------------------------------------------|
| `README.md`                       | Human-facing identity + the Agent Operating Protocol verbatim + PHETAMINE scope |
| `ToDo.md`                         | Roadmap + `Session Memory` section (human-readable log)        |
| `docs/DOCTRINE.md`                | The doctrine (rules this template enforces)                    |
| `docs/CONTEXT.md`                 | This file — repo context                                       |
| `docs/PHETAMINE.md`               | Native-internal design (what "internal" means, corrections)    |
| `docs/ARCHITECTURE.md`            | File-by-file map of `src/`, `ui/`, `tools/`                    |
| `docs/OFFSETS.md`                 | Offset resolution order, dumper schema, probe rules            |
| `docs/UNC_COVERAGE.md`            | UNC tiers with per-function status                             |
| `docs/STATUS.md`                  | Verified here vs UNVERIFIED (Windows-side plan)                |
| `docs/DECISIONS.md`               | ADRs — including why the PlayerListManager trick was replaced  |
| `agent/context.ts`                | **The passthrough.** One typed command replacing ~30 bash calls |
| `agent/memory/session.jsonl`      | Full session log (machine-readable, "everything")              |
| `agent/verification/`             | Capability probes + per-session evidence                       |
| `src/`                            | PHETAMINEAPI.dll (C++20, MSVC x64, `/MT`)                       |
| `ui/PHETAMINEUI/`                 | WPF front end (pipe client, editor, capability panel)          |
| `tools/`                          | Host-side Node tooling (offsets, shellcode blob, verification) |

## Commands

```bash
node agent/context.ts                 # full context brief, logs the run
node agent/context.ts --note "text"   # same + attaches a note to the log entry
node agent/context.ts --probe         # runs capability/tooling probes
node agent/context.ts --json          # raw JSON (for programmatic consumption)
node agent/context.ts --quiet         # brief without doc dumps (big repos)

node tools/verify.mjs                 # static consistency (registry ↔ coverage ↔ offsets)
node tools/selftest.mjs               # runs the tooling self-tests (npm test)
node tools/offsets/fetch_offsets.mjs --version <version-xxxxxxxx>
node tools/gen/shellcode_blob.mjs --self-test
```

## Environment facts

- Node ≥ 22.18 executes `.ts` via type stripping → **zero-dependency
  passthrough**. `npm run ctx` works without `npm install`. Confirm per sandbox
  with `node agent/context.ts --probe`.
- **This sandbox has no `dotnet`, `cl`, `clang`, `cmake`, `mingw-w64`, `wine`,
  or `nasm`** (probed 2026-10-02). The DLL, the shellcode stub, and the WPF UI
  cannot be built or run here; `docs/STATUS.md` §4 is the Windows-side test plan.
- Agent capabilities (image sight, thinking, context tolerance) are verified
  per session in `agent/verification/CAPABILITIES.md`.

## Conventions

- Markdown for humans, JSONL for machines.
- The passthrough must stay dependency-free (Node built-ins only) so it runs on
  any fresh checkout.
- Session log entries are append-only; prune nothing from `session.jsonl`.
  `ToDo.md` keeps only the 12 most recent human-readable lines.
- `tools/verify.mjs` is the guard rail: a C++ registry row without a coverage
  row, an offset key the table does not contain, or a reference to a deleted
  bridge artifact fails the build.
