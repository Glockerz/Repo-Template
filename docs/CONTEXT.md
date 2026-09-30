# Repo Context

Machine-oriented context for agents. Humans see `README.md`; agents read this
plus the passthrough brief.

## Layout

| Path                              | Purpose                                                        |
|-----------------------------------|----------------------------------------------------------------|
| `README.md`                       | Human-facing identity + the Agent Operating Protocol verbatim  |
| `ToDo.md`                         | Roadmap + `Session Memory` section (human-readable log)        |
| `docs/DOCTRINE.md`                | The doctrine (rules this template enforces)                    |
| `docs/CONTEXT.md`                 | This file — repo context                                       |
| `agent/context.ts`                | **The passthrough.** One typed command replacing ~30 bash calls |
| `agent/memory/session.jsonl`      | Full session log (machine-readable, "everything")              |
| `agent/verification/`             | Capability probes + per-session evidence (fresh until first session) |

## Commands

```bash
node agent/context.ts                 # full context brief, logs the run
node agent/context.ts --note "text"   # same + attaches a note to the log entry
node agent/context.ts --probe         # runs capability/tooling probes
node agent/context.ts --json          # raw JSON (for programmatic consumption)
node agent/context.ts --quiet         # brief without doc dumps (big repos)
```

## Environment facts

- Node ≥ 22.18 executes `.ts` via type stripping → **zero-dependency
  passthrough**. `npm run ctx` works without `npm install`. Confirm per sandbox
  with `node agent/context.ts --probe`.
- Other tooling (git, python3, …) is probed per session; nothing is assumed.
- Agent capabilities (image sight, thinking, context tolerance) are verified
  per session in `agent/verification/CAPABILITIES.md` — a fresh template
  starts fully UNVERIFIED.

## Conventions

- Markdown for humans, JSONL for machines.
- The passthrough must stay dependency-free (Node built-ins only) so it runs on
  any fresh checkout.
- Session log entries are append-only; prune nothing from `session.jsonl`.
  `ToDo.md` keeps only the 12 most recent human-readable lines.
