#!/usr/bin/env node
/**
 * agent/context.ts — THE passthrough.
 *
 * One typed command that replaces the legacy ~30-invocation bash ritual
 * (git status / git log / git diff / ls -R / cat README / cat ToDo / cat
 * doctrine / cat context / tail logs / count tokens / check env / …).
 *
 * Run it BEFORE EVERY AGENT RESPONSE. It:
 *   1. parses full repo context (git state, tree, doctrine, docs),
 *   2. reads + appends Session Memory (short-term window shown),
 *   3. emits a problem-solving board (objective / known / risks / open / next).
 *
 * Zero dependencies: Node >= 22.18 executes .ts via type stripping.
 *
 * Usage:
 *   node agent/context.ts [--note "..."] [--quiet] [--json] [--probe]
 *
 * Note: --json is a read-only mode and does NOT append a log entry;
 * every other mode logs the run (rule 3: Session Memory logs everything).
 */

import { execSync } from "node:child_process";
import {
  appendFileSync,
  existsSync,
  mkdirSync,
  readFileSync,
  readdirSync,
  statSync,
  writeFileSync,
} from "node:fs";
import { dirname, join, relative } from "node:path";
import { fileURLToPath } from "node:url";

// ---------------------------------------------------------------- config
const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = join(HERE, "..");
const MEMORY_DIR = join(HERE, "memory");
const SESSION_LOG = join(MEMORY_DIR, "session.jsonl");
const TODO_PATH = join(ROOT, "ToDo.md");

const CONTEXT_BUDGET_TOKENS = 200_000; // agent context tolerance we design for
const SHORT_TERM_WINDOW = 5;          // entries shown as short-term memory
const TODO_MIRROR_LINES = 12;         // human-readable lines kept in ToDo.md
const DOC_MAX_LINES = 60;             // per-doc dump cap in the brief
const TREE_MAX = 60;
const TREE_MAX_DEPTH = 4;
const IGNORED = new Set([
  ".git", "node_modules", "dist", "build", "coverage", ".next", "out", "target",
]);

interface GitState {
  branch: string; sha: string; dirty: string[]; untracked: string[];
  recent: string[];
}
interface MemoryEntry {
  ts: string; branch: string; sha: string; dirty: number; note: string;
}

// ---------------------------------------------------------------- helpers
function sh(cmd: string): string {
  try {
    // trailing-trim only: porcelain lines carry meaningful leading spaces
    // (" M path"); a full trim would corrupt the first line's XY prefix.
    return execSync(cmd, {
      cwd: ROOT, encoding: "utf8", stdio: ["ignore", "pipe", "pipe"],
    }).replace(/\s+$/, "");
  } catch {
    return "";
  }
}

function lines(s: string): string[] {
  return s.length ? s.split("\n") : [];
}

function cap(list: string[], n: number): string[] {
  return list.slice(0, n);
}

function walk(dir: string, depth: number, out: string[]): void {
  if (depth > TREE_MAX_DEPTH) return;
  for (const name of readdirSync(dir).sort()) {
    if (IGNORED.has(name)) continue;
    const p = join(dir, name);
    const st = statSync(p);
    if (st.isDirectory()) walk(p, depth + 1, out);
    else out.push(relative(ROOT, p).split("\\").join("/"));
  }
}

function readDoc(path: string, maxLines = DOC_MAX_LINES): string {
  if (!existsSync(path)) return `_(missing: ${relative(ROOT, path)})_`;
  const all = lines(readFileSync(path, "utf8"));
  const shown = cap(all, maxLines);
  const tail = all.length > shown.length
    ? `\n… (+${all.length - shown.length} more lines)` : "";
  return shown.join("\n") + tail;
}

const estTokens = (s: string): number => Math.ceil(s.length / 4);

// ---------------------------------------------------------------- git
function gitState(): GitState {
  const porcelain = lines(sh("git status --porcelain"));
  const dirty = porcelain.filter((l) => !l.startsWith("??")).map((l) => l.slice(3));
  const untracked = porcelain.filter((l) => l.startsWith("??")).map((l) => l.slice(3));
  return {
    branch: sh("git rev-parse --abbrev-ref HEAD"),
    sha: sh("git rev-parse --short HEAD"),
    dirty, untracked,
    recent: cap(lines(sh("git log --oneline -5")), 5),
  };
}

// ---------------------------------------------------------------- memory
function readMemory(): MemoryEntry[] {
  if (!existsSync(SESSION_LOG)) return [];
  return lines(readFileSync(SESSION_LOG, "utf8"))
    .filter(Boolean)
    .map((l) => { try { return JSON.parse(l) as MemoryEntry; } catch { return null; } })
    .filter((e): e is MemoryEntry => e !== null);
}

function appendMemory(entry: MemoryEntry): void {
  mkdirSync(MEMORY_DIR, { recursive: true });
  appendFileSync(SESSION_LOG, JSON.stringify(entry) + "\n");
  // mirror the last N entries into ToDo.md's Session Memory section
  if (!existsSync(TODO_PATH)) return;
  const todo = readFileSync(TODO_PATH, "utf8");
  const start = todo.indexOf("<!-- session-log:start -->");
  const end = todo.indexOf("<!-- session-log:end -->");
  if (start === -1 || end === -1) return;
  const before = todo.slice(0, start + "<!-- session-log:start -->".length);
  const after = todo.slice(end);
  const all = readMemory();
  const mirror = cap(all.slice(-TODO_MIRROR_LINES), TODO_MIRROR_LINES)
    .map((e) => `- ${e.ts} · ${e.branch}@${e.sha} · ${e.note} (dirty:${e.dirty})`)
    .join("\n");
  writeFileSync(TODO_PATH, `${before}\n${mirror}\n${after}`);
}

// ---------------------------------------------------------------- probes
function runProbes(git: GitState): { name: string; pass: boolean; detail: string }[] {
  const probes: { name: string; pass: boolean; detail: string }[] = [];
  probes.push({
    name: "runtime",
    pass: typeof process.versions.node === "string",
    detail: `node v${process.versions.node} executing .ts via type stripping`,
  });
  const typed: number = 41; // stripped at runtime; proves type-stripping path
  probes.push({
    name: "ts-type-stripping", pass: typed + 1 === 42, detail: "typed const evaluated",
  });
  probes.push({
    name: "git", pass: git.branch.length > 0, detail: `branch=${git.branch} sha=${git.sha}`,
  });
  const tmp = join(ROOT, ".ctx-probe.tmp");
  let fsOk = false;
  try { writeFileSync(tmp, "x"); fsOk = readFileSync(tmp, "utf8") === "x"; } catch { fsOk = false; }
  try { sh(`rm -f ${JSON.stringify(tmp)}`); } catch { /* best effort */ }
  probes.push({ name: "fs-roundtrip", pass: fsOk, detail: "write+read tmp file" });
  probes.push({
    name: "vision-evidence",
    pass: existsSync(join(HERE, "verification", "vision-probe.png")),
    detail: "agent/verification/vision-probe.png present (sight verified by agent, see CAPABILITIES.md)",
  });
  return probes;
}

// ---------------------------------------------------------------- board
interface Board { objective: string; known: string[]; risks: string[]; open: string[]; next: string[]; }

function buildBoard(git: GitState, files: string[], mem: MemoryEntry[]): Board {
  const todo = existsSync(TODO_PATH) ? readFileSync(TODO_PATH, "utf8") : "";
  const openItems = lines(todo)
    .filter((l) => /^\s*- \[ \]/.test(l))
    .map((l) => l.replace(/^\s*- \[ \]\s*/, ""));
  const caps = readDoc(join(HERE, "verification", "CAPABILITIES.md"), 400);
  const openQ = lines(caps).filter((l) => l.startsWith("- UNVERIFIED"))
    .map((l) => l.replace("- UNVERIFIED: ", ""));
  const required = ["README.md", "ToDo.md", "docs/DOCTRINE.md", "docs/CONTEXT.md", "agent/context.ts"];
  const missing = required.filter((r) => !files.includes(r));
  const risks: string[] = [];
  if (git.dirty.length) risks.push(`${git.dirty.length} uncommitted change(s): ${cap(git.dirty, 4).join(", ")}`);
  if (missing.length) risks.push(`missing required files: ${missing.join(", ")}`);
  if (git.branch !== "" && !/^(arena\/|main$)/.test(git.branch))
    risks.push(`off-protocol branch ${git.branch} — work must stay on the session branch`);
  if (!risks.length) risks.push("none detected");
  return {
    objective: openItems[0] ?? "no open objectives — repo is idle",
    known: [
      `branch ${git.branch} @ ${git.sha}`,
      `${files.length} tracked-ish files, ${mem.length} session-log entries`,
      `dirty=${git.dirty.length} untracked=${git.untracked.length}`,
    ],
    risks,
    open: openQ.length ? openQ : ["none recorded"],
    next: cap(openItems, 3),
  };
}

// ---------------------------------------------------------------- main
function main(): void {
  const argv = process.argv.slice(2);
  const wantJson = argv.includes("--json");
  const quiet = argv.includes("--quiet");
  const probe = argv.includes("--probe");
  const ni = argv.indexOf("--note");
  const note = ni !== -1 ? (argv[ni + 1] ?? "") : "";

  const git = gitState();
  const files: string[] = [];
  walk(ROOT, 0, files);
  files.sort();

  const memBefore = readMemory();
  const entry: MemoryEntry = {
    ts: new Date().toISOString(),
    branch: git.branch,
    sha: git.sha,
    dirty: git.dirty.length,
    note: note || (probe ? "probe run" : "context pass"),
  };
  if (!wantJson) appendMemory(entry); // log everything

  const mem = readMemory();
  const shortTerm = mem.slice(-SHORT_TERM_WINDOW);
  const board = buildBoard(git, files, mem);
  const probes = probe ? runProbes(git) : [];

  const docDump = quiet ? "" : [
    "## 3 · Doctrine & context",
    "### README.md", readDoc(join(ROOT, "README.md"), 40), "",
    "### docs/DOCTRINE.md", readDoc(join(ROOT, "docs", "DOCTRINE.md")), "",
    "### docs/CONTEXT.md", readDoc(join(ROOT, "docs", "CONTEXT.md")), "",
    "### ToDo.md", readDoc(TODO_PATH), "",
    "### agent/verification/CAPABILITIES.md", readDoc(join(HERE, "verification", "CAPABILITIES.md")), "",
  ].join("\n");

  let payload = "";
  const out: string[] = [];
  const push = (...s: string[]): void => { out.push(...s); };

  push(`# AGENT CONTEXT BRIEF — ${entry.ts}`);
  push(`> one typed passthrough (node agent/context.ts) — replaces the ~30-command bash ritual`);
  push("");
  push("## 1 · Environment & git");
  push(`node v${process.versions.node} · git branch **${git.branch}** @ ${git.sha} · dirty ${git.dirty.length} · untracked ${git.untracked.length}`);
  push(...git.recent.map((l) => `  ${l}`));
  push("");
  push(`## 2 · Repo tree (${files.length} files)`);
  push(...cap(files, TREE_MAX).map((f) => `  ${f}`));
  if (files.length > TREE_MAX) push(`  … (+${files.length - TREE_MAX} more)`);
  push("");
  if (docDump) { push(docDump); }
  push("## 4 · Session Memory");
  push(`short-term window (last ${shortTerm.length} of ${mem.length} entries; full log: agent/memory/session.jsonl):`);
  push(...shortTerm.map((e) => `  - ${e.ts} · ${e.branch}@${e.sha} · ${e.note}`));
  push("");
  push("## 5 · Problem-Solving Board");
  push(`objective: ${board.objective}`);
  push("known:");
  push(...board.known.map((k) => `  - ${k}`));
  push("risks:");
  push(...board.risks.map((r) => `  - ${r}`));
  push("open questions:");
  push(...board.open.map((o) => `  - ${o}`));
  push("next actions:");
  push(...board.next.map((n) => `  - [ ] ${n}`));
  push("");
  if (probe) {
    push("## 6 · Probes");
    for (const p of probes) push(`  ${p.pass ? "PASS" : "FAIL"} · ${p.name} · ${p.detail}`);
    push("");
  }
  payload = out.join("\n");
  const tokens = estTokens(payload);
  push(`## ${probe ? "7" : "6"} · Context budget`);
  push(`brief ≈ ${payload.length.toLocaleString()} chars ≈ ${tokens.toLocaleString()} tokens ` +
    `(${((tokens / CONTEXT_BUDGET_TOKENS) * 100).toFixed(1)}% of ${CONTEXT_BUDGET_TOKENS.toLocaleString()} budget)`);

  if (wantJson) {
    console.log(JSON.stringify({
      ts: entry.ts, git, files, memory: mem, shortTerm, board,
      probes: probes.length ? probes : undefined,
      budget: { chars: payload.length, estTokens: tokens, budgetTokens: CONTEXT_BUDGET_TOKENS },
    }, null, 2));
    return;
  }
  console.log(out.join("\n"));
  const failed = probes.filter((p) => !p.pass);
  if (failed.length) process.exitCode = 2;
}

main();
