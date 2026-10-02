# Capability Verification Record

Probed **2026-10-02** on branch `arena/01a0fa53-repo-template` @ `1f6995b`.
All four capability classes below are now **VERIFIED**. Evidence is recorded
verbatim (what the probe produced, not what was expected), and the one large
probe artifact lives outside Git per doctrine rule 6.

## Verified — 2026-10-02

- **VERIFIED: tooling** — `node agent/context.ts --probe`, runtime Node v22.22.3
  with `.ts` type stripping (no `npm install`): all 5 probes PASS —
  `runtime`, `ts-type-stripping` (typed const evaluated), `git`
  (`branch=arena/01a0fa53-repo-template sha=1f6995b`), `fs-roundtrip`
  (write+read tmp file), `verification-record` (this file present).

- **VERIFIED: image sight** — generated a tight-spec PNG
  (`agent/verification/vision-probe.png`, committed) and read it back with
  `read_file`. Observed, verbatim: white bold text reading exactly
  **`PROBE OK 4271`** centred on a solid dark-navy background (~`#0B1B2B`);
  beneath it, three solid square swatches in left-to-right order
  **red → green → blue** (~`#C0392B` / `#27AE60` / `#1B3FA8`, i.e. saturated
  primaries, ordering correct). Text and swatch order both matched the prompt.

- **VERIFIED: context tolerance** — wrote a 12,000-line / 887,965-byte corpus to
  `/tmp/ctxprobe/corpus.txt` (**never into Git**, doctrine rule 6) with a sentinel
  embedded mid-file at line 8250:
  `SENTINEL-PHETAMINE-PROBE-4271-MID-FILE`. Read back with
  `read_file(offset=8244, limit=12)` → the sentinel was returned at exactly line
  8250 of 12,001 reported lines, unshifted. The corpus itself is intentionally
  not committed; the evidence is this recorded read-back.

- **VERIFIED: thinking / frontier** — three reasoning probes, answers recorded:
  1. *Literal count* — the letter `r` in `strawberry`: **3**
     (`s-t-r-a-w-b-e-r-r-y`, positions 3, 8, 9).
  2. *Syllogism* — premises "All bloops are razzles" and "No razzles are green":
     conclusion "No bloops are green" is **valid** (razzles and green are
     disjoint, and bloops ⊆ razzles, so bloops ∩ green = ∅).
  3. *Rate invariance* — 60 miles out at 30 mph, 60 miles back at 60 mph:
     average speed over the round trip is **40 mph**, not 45 (harmonic mean of
     the two speeds: `2·30·60 / (30+60)`); total distance 120 mi, total time
     2 h + 1 h = 3 h.

## Unverified

_(none remaining — this section starts empty in fresh templates)_

## Re-probe procedure

Run `node agent/context.ts --probe` after any runtime or sandbox change and
amend this file with the date, the branch/sha, and the results. Re-run the vision
and context-tolerance probes the same way: generate/read a tight-spec PNG, and
write the large corpus to `/tmp` (never the repo) before reading it back at the
sentinel offset.
