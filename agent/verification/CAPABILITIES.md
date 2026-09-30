# Capability Verification Record

Fresh template — **no probes have been run in this repo yet.**
On the first session, the agent must verify each capability below with a live
probe and record dated evidence here (replace the UNVERIFIED lines).

Suggested probes:

- **Image sight**: `generate_image` a PNG from a tight spec (exact text + colors),
  then `read_file` it back and record the exact strings/colors seen.
- **Tooling**: `node agent/context.ts --probe` — expect all PASS; record node/git versions.
- **Context tolerance**: write a large corpus (e.g. 12,000 lines) to `/tmp`
  (never into Git), embed a sentinel mid-file, read it back at the offset.
- **Thinking / frontier**: answer 2–3 reasoning probes (e.g. r-count in
  "strawberry", a syllogism, a rate-invariance puzzle) and record the answers.

## Unverified

- UNVERIFIED: image sight — probe on first session
- UNVERIFIED: tooling — run `node agent/context.ts --probe` on first session
- UNVERIFIED: context tolerance — probe on first session
- UNVERIFIED: thinking/frontier — probe on first session

## Re-probe procedure

Run `node agent/context.ts --probe` after any runtime or sandbox change and
amend this file with the date and results.
