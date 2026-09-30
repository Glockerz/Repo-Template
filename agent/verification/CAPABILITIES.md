# Capability Verification Record

Live probes executed on **2026-09-30** in this sandbox, on branch
`arena/01a0f406-repo-template`. A capability listed here was exercised by a real
tool call in that session; anything not probed is listed under UNVERIFIED.

## Image sight (vision) — VERIFIED

- Probe: `generate_image` wrote `agent/verification/vision-probe.png` from a
  spec; the agent then read the PNG back via `read_file`.
- What the agent reported seeing (exact): grey monospace header text
  `VISION PROBE 0930`; a rounded teal badge with bold white text `CTX-OK 7`;
  below it three circles in order **red, amber/orange, green**; white background.
- All specified elements were read back correctly, including in-image text.
  Text-in-image OCR, color and layout perception: functional.

## Tooling — VERIFIED

- `node v22.22.3`, `npm 10.9.8`, `npx 10.9.8`, `git 2.39.5`, `python3 3.11.2`
  present in sandbox.
- Node executes `.ts` natively via type stripping: probe file printed
  `ts-strip-ok 42` and, with `interface`/type-union annotations, `ts-types-ok ok a`.
- The passthrough itself is the standing tooling proof: `node agent/context.ts --probe`
  returned **5/5 PASS** (runtime, ts-type-stripping, git, fs-roundtrip, vision-evidence).

## Context tolerance — VERIFIED

- Corpus: `/tmp/ctx-corpus.txt`, **12,000 lines / 875,961 bytes** (kept out of Git
  per artifact discipline).
- Mid-file read at offset 6000 returned line 6001 exactly:
  `SENTINEL-CTX-6001-TOLERANCE-PROBE`. Long-context navigation and offset reads: functional.
- The passthrough budgets every brief against a 200,000-token design window
  (first brief ≈ 1,890 tokens ≈ 0.9%).

## Thinking / frontier reasoning — VERIFIED (session-answered probes)

Agent answered these live in the bootstrap session (2026-09-30):
- R-count in "strawberry": **3** (positions 3, 8, 9).
- Syllogism "All Bloops are Razzies; all Razzies are Lazzies ⇒ all Bloops are
  Lazzies": **valid (yes)** — transitivity of subset inclusion.
- Machine/widget rate problem (5 machines/5 min/5 widgets ⇒ 100 machines/100
  widgets): **5 minutes** — per-machine rate is invariant.
- Multi-constraint synthesis: this template itself (doctrine + passthrough +
  memory + verification) was produced in one session from a free-form brief.

## Unverified

- UNVERIFIED: audio/speech generation tools — not required by the protocol; probe on first use.

## Re-probe procedure

Run `node agent/context.ts --probe` after any runtime or sandbox change and
amend this file with the date and results.
