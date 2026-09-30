# Repo-Template

**This repository is the template for every future repo an agent interacts with.**
It is self-describing: doctrine, context, plans, memory, and a capability-verified
single-command context loader all live in the repo, so any fresh agent session can
cold-start, remember, and prove itself before doing work.

## Quick start (for agents)

```bash
node agent/context.ts          # ONE typed passthrough, run before EVERY response
```

That single command replaces the legacy ~30-command bash ritual
(`git status`, `git log`, `git diff`, `ls -R`, `cat` of every doc, tailing logs,
counting tokens, checking the environment, …). It parses the full context, reads
short-term memory, and emits a problem-solving board. See `docs/CONTEXT.md` for
all flags (`--note`, `--quiet`, `--json`, `--probe`).

## Agent Operating Protocol

The instruction this template enforces, verbatim:

> This repo will be a template for every future repo agent mode interacts with.
>
> With the `ToDo.md`, you make another section called **Session Memory**. It logs
> everything, and when you use TypeScript instead of Bash, it compresses that agent
> workflow of running 30 commands into **one TypeScript passthrough** that runs
> before every response. This allows it to parse the full context, short-term
> memory, and problem-solve before responding to you.
>
> Read the doctrine/repo context files; and then verify the model's image-sight
> and tooling capabilities (image reading, thinking, frontier capabilities, and
> context tolerance) before continuing with the next user prompt.

How each clause is implemented:

| Clause | Implementation |
|---|---|
| Template for every future repo | This repo: doctrine (`docs/DOCTRINE.md`), context (`docs/CONTEXT.md`), plans + memory (`ToDo.md`) |
| `ToDo.md` → Session Memory section | `ToDo.md` has `## Session Memory`, mirrored from the full log `agent/memory/session.jsonl` (logs everything) |
| 30 bash commands → one TypeScript passthrough | `agent/context.ts` (zero-dependency, Node type stripping), run before every response |
| Parse full context / short-term memory / problem-solve | The passthrough brief: git + tree + docs, short-term window, problem-solving board |
| Read doctrine/context files | Sections 3 of the brief dump `README.md`, `docs/DOCTRINE.md`, `docs/CONTEXT.md`, `ToDo.md`, capabilities |
| Verify image-sight, thinking, frontier, context tolerance | Live probes; evidence in `agent/verification/CAPABILITIES.md` (vision probe PNG committed) |

## Session lifecycle

1. `node agent/context.ts --note "<why this session started>"` — read the brief.
2. Do the requested work on the session branch only (`arena/<session>-repo-template`).
3. `node agent/context.ts --note "<what was done>"` before ending the turn.

## Layout

```
README.md                      ← you are here (protocol + identity)
ToDo.md                        ← roadmap + Session Memory section
docs/DOCTRINE.md               ← the six rules
docs/CONTEXT.md                ← machine-oriented repo context
agent/context.ts               ← THE passthrough (npm run ctx)
agent/memory/session.jsonl     ← full session log (everything)
agent/verification/            ← vision probe + capability record
```

## Capability verification

This template ships **fresh**: capabilities are unverified until the first agent
session probes them. On its first response the agent runs
`node agent/context.ts --probe` plus the probes listed in
`agent/verification/CAPABILITIES.md` (image sight, tooling, thinking/frontier,
context tolerance) and records dated evidence there. The `open questions` block
of every brief lists whatever is still UNVERIFIED.
