# ToDo

## Template bootstrap

- [x] Scaffold repo (`README.md`, `package.json`, `tsconfig.json`, `.gitignore`)
- [x] Write doctrine (`docs/DOCTRINE.md`)
- [x] Write repo context (`docs/CONTEXT.md`)
- [x] Add `Session Memory` section to `ToDo.md` (this section)
- [x] Build TypeScript passthrough (`agent/context.ts`) — one command replaces the ~30-command bash ritual
- [x] Verify model image sight (vision probe PNG generated + read back)
- [x] Verify tooling (Node type stripping runs the passthrough; git/python present)
- [x] Verify context tolerance (12,000-line corpus, mid-file sentinel read)
- [x] Record capability manifest (`agent/verification/CAPABILITIES.md`)
- [x] Publish the operating protocol into `README.md`

## Ongoing / future

- [ ] Every session: run `node agent/context.ts` before responding (rule 2 of the doctrine)
- [ ] Every session: close the turn with `node agent/context.ts --note "<summary>"`
- [ ] Re-run `--probe` whenever the runtime/sandbox changes; update CAPABILITIES.md

## Session Memory

Logs everything. Human-readable mirror of `agent/memory/session.jsonl`
(the JSONL is the full, unpruned log; this section keeps the 12 latest lines).
The passthrough appends here automatically on every run.

<!-- session-log:start -->
- 2026-09-30T20:39:37.456Z · arena/01a0f406-repo-template@7bacb7f · first probe run during template bootstrap (dirty:0)
- 2026-09-30T20:40:44.269Z · arena/01a0f406-repo-template@7bacb7f · template complete: doctrine+context+ToDo Session Memory+passthrough+README protocol; capabilities verified (dirty:1)
- 2026-09-30T20:41:26.062Z · arena/01a0f406-repo-template@7bacb7f · fixed porcelain leading-space trim bug (EADME.md -> README.md) (dirty:1)
<!-- session-log:end -->
