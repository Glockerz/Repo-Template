# Capability Verification Record

Re-probe procedure: run `node agent/context.ts --probe` after any runtime or
sandbox change and amend this file with the date and results.

## 2026-10-02 · session `arena/01a0fa58-repo-template` (PHETAMINE build)

### Tooling — **PASS**

`node agent/context.ts --probe` → all 5 probes PASS:

| probe | result |
|---|---|
| `runtime` | PASS — node v22.22.3 executes `.ts` via type stripping |
| `ts-type-stripping` | PASS — typed const evaluated |
| `git` | PASS — branch `arena/01a0fa58-repo-template` @ `1f6995b` |
| `fs-roundtrip` | PASS — write+read tmp file |
| `verification-record` | PASS — CAPABILITIES.md present |

Environment: Linux 6.1.158 (e2b), git 2.39.5, python3 3.11.2, g++ (host Linux
only). Brief size ≈ 9,897 chars ≈ 2,475 tokens (1.2% of the 200k design budget).

**Toolchain limits recorded as facts (not assumptions):**

- `dotnet` **absent** → the WPF UI (`ui/PHETAMINEUI`) cannot be built here.
- `clang`, `cl`, `cmake`, `mingw-w64`, `wine`, `nasm` **absent** → no Windows
  PE target, so the C++ DLL and the injected shellcode stub cannot be compiled
  or executed here. `apt-get install` is unavailable (uid 1001, no dpkg lock).
- Consequence: in-sandbox verification covers the *host-portable* tooling
  (`tools/*.mjs`) and static consistency checks only. Everything that needs a
  live Windows/Roblox target is labelled UNVERIFIED in `docs/STATUS.md` and
  must pass the Windows-side test plan before it is trusted.

### Image sight — **PASS**

Probe: `generate_image` with a tight spec (dark navy #0B1020, amber "ARENA-7F3C",
R/G/B labels over red/green/blue squares) → `read_file` of the PNG
(`agent/verification/vision-probe.png`). Read back correctly:

- exact string **`ARENA-7F3C`**, amber, on dark navy ground;
- three squares, left→right **red, green, blue**, each labelled **R, G, B** above;
- layout as specified (text upper half, swatch row below). No misread elements.

### Context tolerance — **PASS**

Probe: 12,000-line / 1,367,814-byte corpus written to `/tmp/ctx-probe/corpus.txt`
(never into Git, per doctrine rule 6), sentinels embedded mid-file and at EOF.
Read back by offset:

- line 6000 → `SENTINEL-MID-7F3C-A1` ✅
- line 12000 → `SENTINEL-END-B19D-4E` ✅

### Thinking / frontier — **PASS**

| probe | answer |
|---|---|
| r-count in "strawberry" | **3** (positions 3, 8, 9) |
| "all bloops are razzies; no razzies are green" → what follows? | **No bloops are green** (valid, Barbara-style chain; its contrapositive "every green thing is a non-bloop" also follows) |
| bat + ball = $1.10, bat is $1.00 more than ball → ball price? | **$0.05** (5 machines/5 widgets → 5 min is the same invariance) |

### Open (unchanged)

- UNVERIFIED: nothing outstanding at template level. Windows/Roblox runtime
  behaviour is tracked per-artifact in `docs/STATUS.md`, not here.
