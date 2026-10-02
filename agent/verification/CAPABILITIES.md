# Capability Verification Record

Probed on **2026-10-02** in session `arena/01a0fa51-repo-template`
(commit base `1f6995b`). Fresh-template UNVERIFIED lines replaced with live
evidence. Re-run `node agent/context.ts --probe` after any sandbox change.

Suggested probes (what was run):

- **Image sight**: `generate_image` a PNG from a tight spec (exact text + colors),
  then `read_file` it back and record the exact strings/colors seen.
- **Tooling**: `node agent/context.ts --probe` — expect all PASS; record node/git versions.
- **Context tolerance**: write a large corpus (e.g. 12,000 lines) to `/tmp`
  (never into Git), embed a sentinel mid-file, read it back at the offset.
- **Thinking / frontier**: answer 2–3 reasoning probes (e.g. r-count in
  "strawberry", a syllogism, a rate-invariance puzzle) and record the answers.

## Verified — 2026-10-02

- **PASS: image sight** — generated `agent/verification/vision-probe.png`
  (512×512, navy `#1A2B4C` field). `read_file` round-trip saw:
  - exact white text `PHETAMINE-PROBE` (centered)
  - exact yellow text `RGB-OK` (`#F5C542`)
  - solid red `#E03131` square, top-left
  - solid green `#2F9E44` circle, bottom-right
  No extra objects or extra text. Vision is trusted for this session.

- **PASS: tooling** — `node agent/context.ts --probe` all PASS:
  - runtime · node **v22.22.3** executing `.ts` via type stripping
  - ts-type-stripping · typed const evaluated
  - git · branch=`arena/01a0fa51-repo-template` sha=`1f6995b` (git **2.39.5**)
  - fs-roundtrip · write+read tmp file
  - verification-record · this file present
  Also present: python3 **3.11.2**, `gh`. `dotnet` / `clang++` not on PATH.

- **PASS: context tolerance** — wrote 12,000-line corpus
  (`/tmp/context-tolerance-corpus.txt`, 504,005 bytes, **not** in Git).
  Sentinel at line 6000 read back exactly:
  `06000 SENTINEL_PHETAMINE_CTX_TOLERANCE_OK_9f3c`.
  Brief budget this session ≈ 2.5k tokens of 200k (1.2%).

- **PASS: thinking / frontier**
  - r-count in "strawberry": **3** (`r` at positions 3, 8, 9)
  - syllogism: All humans are mortal; Socrates is a human → **Socrates is mortal**
  - rate-invariance: 60 mi @ 30 mph then 60 mi @ 90 mph is **not** 60 mph average;
    times are 2 h + ⅔ h = 8/3 h, so average = 120 / (8/3) = **45 mph**
    (harmonic mean of the two speeds).

## Re-probe procedure

Run `node agent/context.ts --probe` after any runtime or sandbox change and
amend this file with the date and results.
