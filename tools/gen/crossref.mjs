#!/usr/bin/env node
// tools/gen/crossref.mjs — offline cross-reference check for src/.
//
// The sandbox this repository is developed in has no C++ toolchain, so the one
// class of mistake that is otherwise invisible — "this call names a function that
// no header declares" and "this struct has no such member" — is checked here by
// parsing the headers instead of compiling.
//
// What it does:
//   1. collects every function/variable declaration from src/**/*.h, grouped by
//      namespace (nested namespaces are flattened to `a::b::name` → name);
//   2. collects the members of known aggregate types (currently `lua::Api`, the
//      one struct whose members are fn pointers called by name);
//   3. scans src/**/*.cpp for `ns::name(` / `ns::name` uses and `api.<member>(`
//      / `GetApi().<member>(` uses and reports anything that is not declared.
//
// It is deliberately heuristic: it does not resolve overloads, templates or
// macros, and it never claims a program compiles. It answers one question —
// "did this name ever get declared?" — and it is exact enough for that.
//
//   node tools/gen/crossref.mjs            # report
//   node tools/gen/crossref.mjs --strict   # exit 1 on any finding
import { readdirSync, readFileSync, statSync } from 'node:fs';
import { join, relative } from 'node:path';

const ROOT = new URL('../..', import.meta.url).pathname.replace(/\/$/, '');
const SRC = join(ROOT, 'src');
const strict = process.argv.includes('--strict');

/** @param {string} dir @param {string[]} out */
function walk(dir, out = []) {
  for (const entry of readdirSync(dir)) {
    const full = join(dir, entry);
    const stat = statSync(full);
    if (stat.isDirectory()) walk(full, out);
    else out.push(full);
  }
  return out;
}

const files = walk(SRC);

// ---- 1. declarations from headers ------------------------------------------
/** @type {Map<string, Set<string>>} namespace → names */
const declared = new Map();
/** @type {Set<string>} */
const allNames = new Set();

function add(ns, name) {
  if (!name || name.length < 2) return;
  if (!declared.has(ns)) declared.set(ns, new Set());
  declared.get(ns).add(name);
  allNames.add(name);
}

// Match things that look like a declarator: `Type name(`, `Type name;`, `name =`,
// trailing `name)` in a using-alias, and fn-pointer fields inside structs.
const DECLARATOR = /(?:^|[\s(,{*&])([A-Za-z_][A-Za-z0-9_]*)\s*(?=[(;=,\[])/gm;

for (const file of files) {
  if (!file.endsWith('.h')) continue;
  const text = readFileSync(file, 'utf8').replace(/\r\n/g, '\n');
  const ns = file.includes('/lua/') || file.includes('/memory/') ? null : null;

  // namespace stack
  let current = [];
  for (const line of text.split('\n')) {
    const nsOpen = line.match(/^\s*namespace\s+([A-Za-z_][A-Za-z0-9_:]*)\s*\{/);
    if (nsOpen) {
      current = nsOpen[1].split('::');
      continue;
    }
    if (/^\s*\}\s*\/\/\s*namespace/.test(line) || /^\s*\}\s*$/.test(line)) {
      if (current.length && /^\s*\}\s*$/.test(line)) current = [];
      continue;
    }
    const stripped = line.replace(/\/\/.*$/, '');
    DECLARATOR.lastIndex = 0;
    let match;
    while ((match = DECLARATOR.exec(stripped)) !== null) {
      const name = match[1];
      if (['return', 'if', 'else', 'while', 'for', 'switch', 'case', 'struct',
           'class', 'enum', 'namespace', 'using', 'typedef', 'inline', 'static',
           'constexpr', 'const', 'void', 'int', 'bool', 'char', 'float', 'double',
           'unsigned', 'signed', 'long', 'short', 'auto', 'extern', 'public',
           'private', 'protected', 'template', 'typename', 'virtual', 'override',
           'explicit', 'operator', 'noexcept', 'constinit', 'static_cast',
           'reinterpret_cast', 'sizeof', 'true', 'false', 'nullptr'].includes(name)) continue;
      add(current.join('::'), name);
      if (current.length) add(current[0], name);
      add('', name);
    }
  }
  void ns;
}

// ---- 2. aggregate members we call by name ----------------------------------
function membersOf(text, name) {
  const members = new Set();
  const start = text.indexOf(`struct ${name}`);
  if (start < 0) return members;
  const body = text.slice(start, text.indexOf('\n};', start));
  for (const match of body.matchAll(/^\s*(?:[A-Za-z_][\w:<>*& ]*\s+)?([a-z_][A-Za-z0-9_]*)\s*(?:=[^;]*)?;/gm)) {
    members.add(match[1]);
  }
  return members;
}

const stateHeader = readFileSync(join(SRC, 'lua/state.h'), 'utf8');
const apiMembers = membersOf(stateHeader, 'Api');

// ---- 3. usages from sources -------------------------------------------------
const NS_RE = /\b([a-z][a-z0-9_]*)::([A-Za-z_][A-Za-z0-9_]*)\s*\(/g;
// namespaces whose headers we do not own or that are external
const EXTERNAL = new Set([
  'mem', 'off', 'seh', 'log', 'str', 'api', 'lua',
  'sched', 'exec', 'ipc', 'net', 'core', 'inject', 'pe', 'scan', 'sigs', 'jobs',
  'rendezvous', 'env', 'identity', 'threads', 'layout', 'net']);

const findings = [];

for (const file of files) {
  if (!file.endsWith('.cpp')) continue;
  const text = readFileSync(file, 'utf8').replace(/\r\n/g, '\n');
  const rel = relative(ROOT, file);
  for (const match of text.matchAll(NS_RE)) {
    const ns = match[1];
    const name = match[2];
    // std:: is the C++ standard library, which is not ours to declare, and any
    // other all-lowercase token that is not one of our namespaces is skipped.
    if (ns === 'std' || !EXTERNAL.has(ns)) continue;
    if (allNames.has(name)) continue;
    findings.push(`${rel}: ${ns}::${name} — no declaration found in any header`);
  }
  // api.<member>( and GetApi().<member>(
  for (const match of text.matchAll(/\bapi\.([a-z_][A-Za-z0-9_]*)\s*\(/g)) {
    if (!apiMembers.has(match[1])) {
      findings.push(`${rel}: api.${match[1]} — not a member of lua::Api (lua/state.h)`);
    }
  }
  for (const match of text.matchAll(/GetApi\(\)\.([a-z_][A-Za-z0-9_]*)/g)) {
    if (!apiMembers.has(match[1])) {
      findings.push(`${rel}: GetApi().${match[1]} — not a member of lua::Api (lua/state.h)`);
    }
  }
}

// ---- report -----------------------------------------------------------------
const unique = [...new Set(findings)];
if (unique.length === 0) {
  console.log(`crossref: ok — ${files.length} source files, no unresolved names`);
  process.exit(0);
}
console.log(`crossref: ${unique.length} finding(s)`);
for (const line of unique) console.log(`  ${line}`);
process.exit(strict ? 1 : 0);
