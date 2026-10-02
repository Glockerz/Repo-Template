# Offsets & signature resolution

A native internal writes through pointers into a live client. A stale offset is
not a failed call — it is a crash. So resolution is a first-class subsystem with
one law:

> **Never guess. An unresolved entry disables its dependent feature and clears
> the capability flag. There is no "plausible default".**

## 1 · Sources, in order

Per entry, at boot (and again per rebind where noted):

1. **Version table** — `src/memory/offsets_embedded.json`, generated from the
   public dumper (`https://offsets.imtheo.lol`) for a specific
   `version-<hash>`. Fast path, zero scan cost.
2. **AOB signature scan** — `src/memory/sigs.h`, patterns derived per build.
   Used when the table is stale, unknown, or an entry is zero.
3. **Offset fallback** — the table value used only if its **runtime probe**
   passes.
4. **Disable** — capability false, dependent functions not registered, one
   `ERROR:Offsets:<key>` line logged.

Every resolved value is *probed* before first use; the probe result is cached for
the session and re-run after a rebind.

## 2 · The dumper's schema (observed 2026-10-02)

`offsets.json` is decimal, not hex, and looks like:

```json
{
  "Source": "https://offsets.imtheo.lol",
  "Roblox Version": "version-02c37bc51a384b8f",
  "Dumper Version": "2.2.4",
  "Dumped At": "20:12 29/09/2026",
  "Total Offsets": 392,
  "Offsets": {
    "TaskScheduler": { "Pointer": 145748640, "JobStart": 200, "JobEnd": 208, "JobName": 24, "MaxFPS": 176 },
    "FakeDataModel": { "Pointer": 146098560, "RealDataModel": 504 },
    "DataModel": {
      "ScriptContext": 1088, "JobId": 272, "PlaceId": 392, "GameId": 384,
      "Workspace": 336, "GameLoaded": 1488, "ToRenderView1": 448
    },
    "Instance": {
      "NameContainer": 112, "Name": 8, "ChildrenStart": 120, "ChildrenEnd": 8,
      "Parent": 104, "ClassDescriptor": 24, "ClassName": 8, "ClassBase": 432
    },
    "Misc": { "StringLength": 16 },
    "ScriptContext": { "RequireBypass": 0 }
  }
}
```

Rules this schema forces on us:

- **Values are decimal** — `map_offsets.mjs` converts; the `.inc` header carries
  hex with the source namespace in a comment.
- **`0` means "not dumped / unavailable"**, not "offset zero". `0` resolves to
  *absent*. (`ScriptContext.RequireBypass: 0` above is an example: the dumper had
  no value, and pretending otherwise is exactly the bug this rule prevents.)
- **Only what is published is usable.** The dumper covers instance/data fields.
  There is no `lua_State`, no `luau_compile`, no identity entry — the VM layer
  is signature + probe territory, always.
- **Key names embed their base**: `FakeDataModel.Pointer` is an image-relative
  pointer (`base + value`), while `FakeDataModel.RealDataModel` is an offset
  from the resolved FakeDataModel pointer. `map_offsets.mjs` keeps that
  distinction in the generated header (`kind: rva | offset`).

## 3 · Evidence that hardcoding is wrong

Two public sources for two different builds, both internally consistent:

| field | build `version-d599f7fc52a8404c` (`srcsyntax`) | build `version-02c37bc51a384b8f` (dumper) |
|---|---|---|
| name read | `instance + 0xB0` (SSO struct) | `NameContainer = 0x70`, `Name = 0x08` within it |
| children | `container + 0x70`, `[start, end)` | `ChildrenStart = 0x78` |
| parent | `+0x68` | `Parent = 0x68` (unchanged) |
| ClassDescriptor | `+0x18` | `ClassDescriptor = 0x18` (unchanged) |

Parent and class descriptor survived; the name/children pair moved. A hardcoded
`0xB0` compiles, runs, and reads garbage on the other build. Hence: table +
probe, and a `walker` that validates the first read of each shape before use
(`IsValidInstance`, class-name compare, max child cap).

## 4 · VM-layer resolution (table has nothing for us)

| What | How it is found | Probe that must pass |
|---|---|---|
| `ScriptContext` | `DataModel.ScriptContext` (table) | class name == `"ScriptContext"` |
| global `lua_State*` | scan ScriptContext pointer slots for a pointer that passes the state probe; then AOB; then table offset | `ProbeState`: globals readable, `game`/`print`/`task` present, `lua_ref` round-trip |
| `luau_compile` | AOB in `.text` | compiles a known string; bytecode version matches `luau_load` |
| `luau_load` | AOB | loads the canary chunk and runs it |
| `lua_resume`/`lua_pcall`/`lua_newthread`/`lua_pushcclosurek`/`lua_ref`/`lua_unref`/`lua_setfield` | AOB | canary at identity 8 executes end-to-end |
| client `free` (for `luau_compile` buffers) | AOB / CRT resolution in-process | compile → free → compile again, no corruption |
| identity field in extra space | offset + AOB | `set 8 → read back 8` on a thread we own, and unchanged on engine threads |
| namecall `TString*` | offset + AOB | round-trip a known `:method` call |
| GC list head / registry | offset + AOB | bounded walk returns only valid objects (empty on doubt) |
| ScriptContext task-queue insert | AOB of the enqueue helper | our drain is called once per frame, heartbeat advances |

## 5 · Update runbook (client patched)

1. `node tools/offsets/fetch_offsets.mjs --version <version-hash>` → new
   `offsets_embedded.json` (records `Source`/`Dumped At` headers verbatim).
2. `node tools/offsets/gen_offsets_header.mjs` → `offsets_table.inc`; it refuses
   unknown keys, so a renamed namespace is a build error, not silent zero.
3. Build with `-DPHETAMINE_DUMP_ON_BOOT=ON`, attach, read `INFO` + the `dump`
   lines: every resolved address and its probe result.
4. Re-derive the AOB set in `memory/sigs.h` for anything the probe failed;
   paste the addresses the dump reported into the table if a scan is unwanted.
5. `node tools/verify.mjs` — confirms every capability in the registry has a
   probe path and a coverage row.
6. Soak: teleport ×2, rejoin, canary, UNC suite. Any probe failure with a
   non-zero table value means the entry changed meaning — re-dump, do not
   "adjust": the probe is the contract.
