# Doctrine

This repository is the **template for every future repo an agent interacts with**.
Everything in it exists so that a fresh agent session can cold-start in one command,
remember what happened, and prove its own capabilities before doing work.

## The six rules

1. **Self-describing repo.** Doctrine, context, and plans live *in the repo*
   (`docs/DOCTRINE.md`, `docs/CONTEXT.md`, `ToDo.md`). An agent never has to guess
   conventions; it reads them.

2. **One passthrough, not thirty commands.** The legacy workflow of running
   ~30 bash commands (`git status`, `git log`, `ls -R`, `cat` every doc, tailing
   logs, counting tokens, …) is compressed into a single TypeScript passthrough:

   ```bash
   node agent/context.ts          # or: npm run ctx
   ```

   It is run **before every agent response**. It parses full context, reads
   short-term memory, and emits a problem-solving board.

3. **Session Memory logs everything.** Every passthrough run appends a dated,
   machine-readable entry to `agent/memory/session.jsonl` and a human-readable
   line to the `Session Memory` section of `ToDo.md`. Nothing a session did is
   lost; the next session inherits it.

4. **Verify before trusting.** Image sight, tooling, thinking, and context
   tolerance are verified with live probes (`agent/verification/`) and recorded in
   `agent/verification/CAPABILITIES.md`. A capability that has not been probed is
   reported as *unverified*, never assumed.

5. **Branch discipline.** Work happens only on the session branch
   (`arena/<session>-repo-template`). Never switch to, create, or push any other
   branch. Commit and push to it only.

6. **Artifact discipline.** Keep generated bloat and large datasets out of Git.
  Probes that need big corpora write them to `/tmp`, never into the repo. Small
  evidence artifacts (probe images, capability records) are committed.

## Session lifecycle (what every future session does)

1. `node agent/context.ts --note "<why this session started>"`
2. Read the emitted brief (context + short-term memory + problem board).
3. Do the work the user asked for.
4. `node agent/context.ts --note "<what was done>"` before ending the turn.
