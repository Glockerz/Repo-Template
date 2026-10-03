# ToDo

## Tasks

Add your tasks below — one per line.
`- [ ]` = open (the agent picks these up as objectives), `- [x]` = done.

<!-- example, copy it or delete it:
- [ ] my first task
-->

## Session Memory

Logged automatically by `node agent/context.ts` — the 12 most recent entries
are mirrored here; the full unpruned log lives in `agent/memory/session.jsonl`.

<!-- session-log:start -->
- 2026-10-03T12:35:05.552Z · arena/01a101c2-repo-template@12aedce · Started deobfuscating deobfuscate.lua using Glockerz/luau as the requested Luau runtime reference. (dirty:0)
- 2026-10-03T12:42:41.054Z · arena/01a101c2-repo-template@12aedce · Deobfuscated the Prometheus VM into readable Luau; identified and disabled a hard-coded telemetry webhook, preserved documented gambling logic, command handling, queues, and the two-user favorable-roll backdoor for auditability. (dirty:2)
- 2026-10-03T12:46:28.240Z · arena/01a101c2-repo-template@12aedce · Preparing the completed Luau deobfuscation for commit and push to the fixed Arena session branch. (dirty:2)
- 2026-10-03T13:08:37.512Z · arena/01a101c2-repo-template@f3174d2 · Started planning a functional Roblox UI and configurable automatic bank-player transfers for the deobfuscated gambling bot. (dirty:0)
- 2026-10-03T13:13:59.501Z · arena/01a101c2-repo-template@f3174d2 · Added and validated a draggable Roblox auto-bank UI with in-server player selection, editable transfer trigger/reserve balances, default-disabled controls, live balance/profit/status display, RightShift/minimize controls, queued transfers, and stale-balance duplicate prevention. (dirty:3)
- 2026-10-03T13:16:03.777Z · arena/01a101c2-repo-template@f3174d2 · Started adding a single-instance execution guard and executor-filesystem save/load configuration modeled after the provided Auto Suite pattern. (dirty:3)
- 2026-10-03T13:21:52.843Z · arena/01a101c2-repo-template@f3174d2 · Completed single-instance replacement and named JSON config persistence: prior runtime loops/connections/UI are stopped and cleaned, config Save/Load refreshes banking controls, and an existing default config auto-loads with offline-recipient safety. Validation passed Luau compilation, diff checks, mocked replacement cleanup, named save/load, default auto-load, default-disabled zero-transfer, and exactly-one reserve transfer tests. (dirty:3)
<!-- session-log:end -->
