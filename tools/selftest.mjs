#!/usr/bin/env node
/**
 * tools/selftest.mjs — runs every host-runnable check in one go (`npm test`).
 *
 * This is deliberately narrow: it exercises the tooling that CAN run in this
 * sandbox (Node-only), and never pretends to build or run the Windows payload.
 * docs/STATUS.md §4 lists what only a Windows host can verify.
 */

import { execFileSync } from "node:child_process";
import { existsSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const node = process.execPath;

const steps = [
  { name: "shellcode blob parser (synthetic PEs)", cmd: "tools/gen/shellcode_blob.mjs", args: ["--self-test"] },
  { name: "offsets pipeline (fixture → embedded)", cmd: "tools/offsets/fetch_offsets.mjs",
    args: ["--from", "tools/offsets/__fixtures__/observed_02c37bc51a384b8f.json", "--out", "/tmp/phetamine-offsets-selftest.json"] },
  { name: "offsets header validation", cmd: "tools/offsets/fetch_offsets.mjs",
    args: ["--from", "tools/offsets/__fixtures__/observed_02c37bc51a384b8f.json", "--out", "src/memory/offsets_embedded.json"] },
  { name: "static consistency (verify.mjs)", cmd: "tools/verify.mjs", args: [] },
  { name: "module image contract (check_module)", cmd: "tools/gen/check_module.mjs", args: ["--self-test"] },
  { name: "source cross-reference (crossref)", cmd: "tools/gen/crossref.mjs", args: ["--strict"] },
];

let allPass = true;
for (const step of steps) {
  const cmd = [resolve(ROOT, step.cmd), ...step.args];
  try {
    const out = execFileSync(node, cmd, { cwd: ROOT, encoding: "utf8", stdio: ["ignore", "pipe", "pipe"] });
    const lines = (out.trim().split("\n").filter(Boolean)).slice(-1)[0] ?? "(no output)";
    console.log(`PASS · ${step.name} · ${lines}`);
  } catch (err) {
    allPass = false;
    const out = `${err.stdout ?? ""}${err.stderr ?? ""}`.trim();
    console.log(`FAIL · ${step.name} · ${out.split("\n").slice(-4).join(" | ")}`);
  }
}

// every required path exists
const required = [
  "README.md", "ToDo.md", "docs/DOCTRINE.md", "docs/CONTEXT.md", "docs/PHETAMINE.md",
  "docs/ARCHITECTURE.md", "docs/OFFSETS.md", "docs/UNC_COVERAGE.md", "docs/STATUS.md", "docs/DECISIONS.md",
  "src/native_api/registry.def", "src/ipc/protocol.h", "ui/PHETAMINEUI/PipeClient.cs",
  "agent/verification/CAPABILITIES.md", "agent/verification/vision-probe.png",
];
const missing = required.filter((p) => !existsSync(resolve(ROOT, p)));
if (missing.length) { allPass = false; console.log(`FAIL · required paths · missing: ${missing.join(", ")}`); }
else console.log(`PASS · required paths · ${required.length} present`);

process.exitCode = allPass ? 0 : 1;
