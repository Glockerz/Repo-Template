#!/usr/bin/env node
/**
 * tools/verify.mjs — static consistency checks for the PHETAMINE tree.
 *
 * These are the invariants that a compiler cannot see, and that a Windows-only
 * build would otherwise let rot:
 *
 *   1. every native function in registry.def has a row in docs/UNC_COVERAGE.md;
 *   2. every capability symbol used by registry.def has a name/description row
 *      in src/native_api/api.h (that pair is what PHETAMINE.capability() and
 *      the UI panel both read);
 *   3. every offset key the C++ asks for exists in the generated table;
 *   4. the IPC opcode tables in src/ipc/protocol.h and ui/PHETAMINEUI/PipeClient.cs
 *      agree exactly (name *and* value) — a mismatch is a silently broken pipe;
 *   5. no deleted bridge artifact is referenced from src/ or ui/ (they are only
 *      allowed to appear in docs/, as history).
 *
 * Usage: node tools/verify.mjs [--json]
 */

import { readFileSync, existsSync, readdirSync, statSync } from "node:fs";
import { dirname, join, resolve, relative } from "node:path";
import { fileURLToPath } from "node:url";
import { KEYMAP } from "./offsets/map_offsets.mjs";

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = resolve(HERE, "..");

const P = {
  registry: resolve(ROOT, "src/native_api/registry.def"),
  apiHeader: resolve(ROOT, "src/native_api/api.h"),
  coverage: resolve(ROOT, "docs/UNC_COVERAGE.md"),
  offsets: resolve(ROOT, "src/memory/offsets_embedded.json"),
  protocol: resolve(ROOT, "src/ipc/protocol.h"),
  pipeClient: resolve(ROOT, "ui/PHETAMINEUI/PipeClient.cs"),
};

/** strings that only exist in the deleted bridge design; src/ui must not mention them */
const DELETED_ARTIFACTS = [
  "http_server", "GetInitScript", "unc_payload", "RSB1Encoder",
  "InjectViaSpoof", "InjectViaModuleAnchor", "InjectViaLuauState",
  "FindUnloadedModule", "SetBytecode", "RestoreAllModules",
  "SimulateEscKey", "RedirConsole", "ModifiedModule", "PlayerListManager",
  "127.0.0.1:9753",
];

const results = [];
const pass = (check, detail) => results.push({ check, pass: true, detail });
const fail = (check, detail) => results.push({ check, pass: false, detail });

function readIfExists(p) {
  return existsSync(p) ? readFileSync(p, "utf8") : null;
}

function walkFiles(dir, exts, out = []) {
  if (!existsSync(dir)) return out;
  for (const name of readdirSync(dir)) {
    const p = join(dir, name);
    const st = statSync(p);
    if (st.isDirectory()) walkFiles(p, exts, out);
    else if (exts.some((e) => name.endsWith(e))) out.push(p);
  }
  return out;
}

// ---------------------------------------------------------------- 1 · registry ↔ coverage
function checkRegistryCoverage() {
  const reg = readIfExists(P.registry);
  const cov = readIfExists(P.coverage);
  if (!reg) return fail("registry.def exists", `${relative(ROOT, P.registry)} missing`);
  if (!cov) return fail("UNC_COVERAGE.md exists", `${relative(ROOT, P.coverage)} missing`);

  // Row format: FN(c_symbol, "lua.name", translation_unit, Cap::Capability)
  const rows = [...reg.matchAll(
    /^\s*FN\(\s*([A-Za-z_][\w.]*)\s*,\s*"([\w.]+)"\s*,\s*(\w+)\s*,\s*Cap::(\w+)\s*\)/gm,
  )].map((m) => ({ symbol: m[1], name: m[2], tu: m[3], cap: m[4] }));
  if (rows.length === 0) return fail("registry.def parses", "no FN() rows found");

  // UNC_COVERAGE.md documents the *Lua* names, because that is what a script
  // author searches for.
  const missing = rows.filter((r) => !cov.includes("`" + r.name + "`")).map((r) => r.name);
  const dupes = rows.map((r) => r.name).filter((n, i, a) => a.indexOf(n) !== i);
  if (missing.length) fail("every registered function is documented", `missing from UNC_COVERAGE.md: ${missing.join(", ")}`);
  else pass("every registered function is documented", `${rows.length} entries`);
  if (dupes.length) fail("registry has no duplicate names", dupes.join(", "));
  else pass("registry has no duplicate names", `${rows.length} unique`);

  // every row's c_symbol must be implemented as l_<symbol> in its TU
  const unimplemented = [];
  for (const r of rows) {
    const tuPath = resolve(ROOT, `src/native_api/${r.tu}.cpp`);
    if (!existsSync(tuPath)) continue;   // reported by the TU check below
    const text = readFileSync(tuPath, "utf8");
    if (!new RegExp(`\\bl_${r.symbol}\\s*\\(`).test(text)) {
      unimplemented.push(`${r.symbol} (expected l_${r.symbol} in ${r.tu}.cpp)`);
    }
  }
  if (unimplemented.length) fail("every registry row has an implementation", unimplemented.join("; "));
  else pass("every registry row has an implementation", `${rows.length} symbols`);

  // TUs used in registry.def must have a file
  const tus = [...new Set(rows.map((r) => r.tu))];
  const badTu = tus.filter((tu) => !existsSync(resolve(ROOT, `src/native_api/${tu}.cpp`)));
  if (badTu.length) fail("every registry TU has an implementation", badTu.join(", "));
  else pass("every registry TU has an implementation", tus.join(", "));

  return { rows };
}

// ---------------------------------------------------------------- 2 · capabilities
function checkCapabilities(registry) {
  const api = readIfExists(P.apiHeader);
  if (!api) return fail("api.h exists", `${relative(ROOT, P.apiHeader)} missing`);

  const declared = new Set([...api.matchAll(/\{\s*"([a-z0-9_.]+)"\s*,\s*Cap::(\w+)\s*,/g)].map((m) => m[2]));
  const used = new Set(registry ? registry.rows.map((r) => r.cap) : []);
  const undeclared = [...used].filter((c) => !declared.has(c));
  if (undeclared.length) fail("every capability used is declared", `not in api.h kCapabilities: ${undeclared.join(", ")}`);
  else pass("every capability used is declared", `${declared.size} declared, ${used.size} used`);

  // comments inside the enum carry commas, so they have to go before splitting
  const enumNames = new Set([...api.matchAll(/enum class Cap[^{]*\{([^}]*)\}/gs)]
    .flatMap((m) => m[1].replace(/\/\/[^\n]*/g, " ").split(",")
      .map((s) => s.trim().split("=")[0].trim())
      .filter((s) => /^[A-Za-z_]\w*$/.test(s))));
  const missingEnum = [...used].filter((c) => !enumNames.has(c));
  if (missingEnum.length) fail("capability symbols exist in enum Cap", missingEnum.join(", "));
  else pass("capability symbols exist in enum Cap", `${enumNames.size} enumerators`);
}

// ---------------------------------------------------------------- 3 · offset keys
function checkOffsetKeys() {
  const off = readIfExists(P.offsets);
  if (!off) return fail("offsets_embedded.json exists", `${relative(ROOT, P.offsets)} missing`);
  let table;
  try { table = JSON.parse(off); } catch (e) { return fail("offsets_embedded.json parses", e.message); }
  const keys = new Set(Object.keys(table.offsets ?? {}));

  const used = new Map();
  for (const f of walkFiles(resolve(ROOT, "src"), [".cpp", ".h", ".inc"])) {
    const text = readFileSync(f, "utf8");
    for (const m of text.matchAll(/off::Get\(\s*"([a-z0-9_.]+)"\s*\)/g)) {
      if (!used.has(m[1])) used.set(m[1], relative(ROOT, f));
    }
  }
  const unknown = [...used.entries()].filter(([k]) => !keys.has(k));
  if (unknown.length) fail("every offset key the code asks for exists in the table",
    unknown.map(([k, f]) => `${k} (${f})`).join("; "));
  else pass("every offset key the code asks for exists in the table", `${used.size} keys referenced, ${keys.size} in table`);

  // keys in the table must be known to the mapping tool
  const unmapped = [...keys].filter((k) => !(k in KEYMAP));
  if (unmapped.length) fail("every table key is in map_offsets KEYMAP", unmapped.join(", "));
  else pass("every table key is in map_offsets KEYMAP", `${keys.size} keys`);
}

// ---------------------------------------------------------------- 4 · IPC opcodes
function checkIpc() {
  const proto = readIfExists(P.protocol);
  const cs = readIfExists(P.pipeClient);
  if (!proto) return fail("protocol.h exists", `${relative(ROOT, P.protocol)} missing`);
  if (!cs) return fail("PipeClient.cs exists", `${relative(ROOT, P.pipeClient)} missing`);

  const cppOps = [...proto.matchAll(/^\s*X\(\s*(0x[0-9A-Fa-f]+)\s*,\s*(\w+)\s*\)/gm)]
    .map((m) => ({ value: parseInt(m[1], 16), name: m[2] }));
  const csOps = [...cs.matchAll(/^\s*(\w+)\s*=\s*(0x[0-9A-Fa-f]+)\s*,/gm)]
    .map((m) => ({ name: m[1], value: parseInt(m[2], 16) }));

  if (!cppOps.length) return fail("protocol.h opcode table parses", "no X(op, Name) rows");
  if (!csOps.length) return fail("PipeClient.cs opcode table parses", "no Name = 0x.. rows");

  const cppMap = new Map(cppOps.map((o) => [o.name, o.value]));
  const csMap = new Map(csOps.map((o) => [o.name, o.value]));
  const problems = [];
  for (const [name, value] of cppMap) {
    if (!csMap.has(name)) problems.push(`${name} (0x${value.toString(16)}) missing in PipeClient.cs`);
    else if (csMap.get(name) !== value) problems.push(`${name} value mismatch: C++ 0x${value.toString(16)} vs C# 0x${csMap.get(name).toString(16)}`);
  }
  for (const [name, value] of csMap) {
    if (!cppMap.has(name)) problems.push(`${name} (0x${value.toString(16)}) missing in protocol.h`);
  }
  if (problems.length) fail("IPC opcode tables agree", problems.join("; "));
  else pass("IPC opcode tables agree", `${cppOps.length} opcodes, values match`);
}

// ---------------------------------------------------------------- 5 · deleted artifacts
function checkDeletedArtifacts() {
  const hits = [];
  for (const f of walkFiles(resolve(ROOT, "src"), [".cpp", ".h", ".c", ".inc"])) {
    const text = readFileSync(f, "utf8");
    for (const needle of DELETED_ARTIFACTS) {
      if (text.includes(needle)) hits.push(`${relative(ROOT, f)} mentions "${needle}"`);
    }
  }
  for (const f of walkFiles(resolve(ROOT, "ui"), [".cs", ".xaml"])) {
    const text = readFileSync(f, "utf8");
    for (const needle of DELETED_ARTIFACTS) {
      if (text.includes(needle)) hits.push(`${relative(ROOT, f)} mentions "${needle}"`);
    }
  }
  if (hits.length) fail("no deleted bridge artifact in src/ or ui/", hits.join("; "));
  else pass("no deleted bridge artifact in src/ or ui/", `${DELETED_ARTIFACTS.length} needles, 0 hits`);
}

// ---------------------------------------------------------------- main
function main(argv) {
  const json = argv.includes("--json");
  const registry = checkRegistryCoverage();
  checkCapabilities(registry);
  checkOffsetKeys();
  checkIpc();
  checkDeletedArtifacts();

  if (json) {
    console.log(JSON.stringify({ results, ok: results.every((r) => r.pass) }, null, 2));
    return results.every((r) => r.pass) ? 0 : 1;
  }
  for (const r of results) console.log(`${r.pass ? "PASS" : "FAIL"} · ${r.check} · ${r.detail}`);
  const failed = results.filter((r) => !r.pass).length;
  console.log(`\n${results.length - failed}/${results.length} checks passed`);
  return failed ? 1 : 0;
}

process.exitCode = main(process.argv.slice(2));
