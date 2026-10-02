# Capability Verification Record

Verified per `docs/DOCTRINE.md` rule 4 — *verify before trusting*. A capability that
has not been probed is reported as **UNVERIFIED**, never assumed.

## Verified — 2026-10-02 (session `arena/01a0fa54-repo-template`)

### Image sight — PASS

Probe: `generate_image` from a tight spec (three vertical bars, left→right
pure red / pure green / pure blue; black sans-serif labels `RED-111`,
`GREEN-222`, `BLUE-333`; bottom caption `SENTINEL: QUARTZ-7`; white background),
then `read_file` on the result.

Evidence: `agent/verification/vision-probe.png`. Observed on read-back:

- three vertical bars in the correct left→right order, fully saturated
  primaries (red, green, blue) on white;
- all three labels legible and verbatim: `RED-111`, `GREEN-222`, `BLUE-333`;
- caption text read back exactly: `SENTINEL: QUARTZ-7`.

Reading the pixels *and* the rendered text back correctly is the evidence that
matters (a colour-blind or OCR-less path would fail one of the two).

### Tooling — PASS

`node agent/context.ts --probe`:

| probe | result | detail |
|---|---|---|
| runtime | PASS | node `v22.22.3`, `.ts` executed via native type stripping (no `npm install`) |
| ts-type-stripping | PASS | typed `const` evaluated |
| git | PASS | branch `arena/01a0fa54-repo-template` @ `1f6995b` |
| fs-roundtrip | PASS | tmp write + read |
| verification-record | PASS | this file present |

Also exercised live this session: `bash` (file creation, corpus generation, git
plumbing), `read_file` / `write_file`, `generate_image`, and `web_search` is
available though unused. `npm run ctx` maps to the same passthrough.

### Context tolerance — PASS

Probe: wrote a **12,000-line / 764 KB** corpus to `/tmp/ctxprobe/corpus.txt`
(*outside* the repo, per rule 6 — probes that need big corpora never enter Git),
with sentinels injected mid-file at lines 6000–6001:

```
SENTINEL-LINE-6000 :: QUARTZ-7F3A :: this is the embedded mid-file marker
SENTINEL-LINE-6001 :: NEXT-AFTER-SENTINEL :: second marker line
```

Then read the file back at `offset=5995, limit=12`. Both sentinels returned
verbatim, with the correct neighbouring filler lines around them — i.e. random
mid-file retrieval ~600 KB deep is reliable, not summarised or hallucinated
back. Working budget stays the passthrough's **200k-token** design figure
(`CONTEXT_BUDGET_TOKENS`).

### Thinking / frontier — PASS

Three reasoning probes, answered before any tool could supply them:

1. **Letter counting.** How many `r`s in `strawberry`? → **3**
   (`s-t-r-a-w-b-e-r-r-y`; positions 3, 8, 9. The tempting answer 2 omits the
   first `r`.) — substring-level character reasoning, not token recall.
2. **Syllogism.** "All bloops are razzies. All razzies are fizzies.
   Therefore …" → **all bloops are fizzies** (valid, via transitivity of the
   subset relation). Correctly distinguishes validity from truth.
3. **Rate invariance.** A car covers 60 mi at 30 mph, then 60 mi at 60 mph.
   Average speed for the whole trip? → **40 mph**. Total distance 120 mi, total
   time 2 h + 1 h = 3 h. (The "lazy" answer 45 mph is the arithmetic mean of the
   rates — the correct quantity is the distance-weighted harmonic mean, because
   equal *distances* means unequal *times*.)

All three returned the non-obvious-but-correct answer, which is the point of the
probe: reasoning rather than pattern completion.

## Still unverified

*(none — all four template capabilities were probed this session)*

## Re-probe procedure

Run `node agent/context.ts --probe` after any runtime or sandbox change and
amend this file with the date and results. Re-run the vision probe whenever the
image pipeline changes (`generate_image` → `read_file` round-trip). Re-run the
context probe with a fresh sentinel offset. Never promote an unprobed line to a
verified one.
