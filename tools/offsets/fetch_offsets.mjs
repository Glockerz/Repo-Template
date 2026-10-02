#!/usr/bin/env node
/**
 * tools/offsets/fetch_offsets.mjs
 *
 * Refresh src/memory/offsets_embedded.json from a theo's-offsets dump.
 *
 * Two input modes:
 *   --version <version-xxxxxxxx>   fetch from https://offsets.imtheo.lol
 *   --from <file.json>             use a dump already on disk (offline / CI)
 *
 * Output is produced by map_offsets.mjs (single normalisation path — never two).
 *
 * Usage:
 *   node tools/offsets/fetch_offsets.mjs --version version-02c37bc51a384b8f
 *   node tools/offsets/fetch_offsets.mjs --from dump.json --out src/memory/offsets_embedded.json
 *   node tools/offsets/fetch_offsets.mjs --from dump.json --stdout
 */

import { readFileSync, writeFileSync, mkdirSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { normalise } from "./map_offsets.mjs";

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = resolve(HERE, "..", "..");
const BASE = "https://offsets.imtheo.lol";
const DEFAULT_OUT = resolve(ROOT, "src/memory/offsets_embedded.json");

async function fetchDump(version) {
  const url = `${BASE}/${version}/offsets.json`;
  const res = await fetch(url, { headers: { accept: "application/json" } });
  if (!res.ok) throw new Error(`GET ${url} → HTTP ${res.status}`);
  return await res.json();
}

function main(argv) {
  const arg = (flag, dflt) => {
    const i = argv.indexOf(flag);
    return i === -1 ? dflt : argv[i + 1];
  };
  const from = arg("--from");
  const version = arg("--version");
  const out = arg("--out", DEFAULT_OUT);
  const toStdout = argv.includes("--stdout");
  const strict = !argv.includes("--no-strict");

  if (!from && !version) {
    console.error("usage: fetch_offsets.mjs (--version <version-x> | --from <dump.json>) [--out <file>] [--stdout] [--no-strict]");
    return 2;
  }

  const load = async () => {
    if (from) return JSON.parse(readFileSync(from, "utf8"));
    try {
      return await fetchDump(version);
    } catch (err) {
      console.error(`fetch failed: ${err.message}`);
      console.error("If this sandbox blocks egress, download the dump elsewhere and pass --from <file>.");
      process.exitCode = 3;
      return null;
    }
  };

  return (async () => {
    const dump = await load();
    if (!dump) return 3;
    const mapped = normalise(dump, { strict });
    const text = JSON.stringify(mapped, null, 2) + "\n";
    if (toStdout) { process.stdout.write(text); return 0; }
    mkdirSync(dirname(out), { recursive: true });
    writeFileSync(out, text);
    console.error(`wrote ${out} — ${Object.keys(mapped.offsets).length} keys for ${mapped.roblox_version}`);
    return 0;
  })();
}

process.exitCode = await main(process.argv.slice(2));
