#!/usr/bin/env node
/**
 * tools/gen/shellcode_blob.mjs
 *
 * Turn the linked stub (a PE with the loader code in .text) into a raw
 * `stub_blob.inc`: the bytes the injector writes into the target process.
 *
 * The stub is never "loaded" by the OS — we copy its .text into a staging
 * allocation and jump to it. That means the Windows loader will NOT apply
 * base relocations, so the blob must be genuinely position-independent:
 *
 *   - the entry RVA must live inside .text;
 *   - .reloc must contain no DIR64 entry pointing into .text (an absolute
 *     address baked into code = a crash the moment the stub is not at its
 *     preferred base — which it never is);
 *   - any relocation at all is a smell: the stub keeps string and table data
 *     in .text and addresses it RIP-relative. Relocations outside .text are
 *     rejected too, with their own message, because we have nowhere to apply
 *     them;
 *   - trailing alignment padding (0xCC / 0x00) is trimmed off the end.
 *
 * Usage:
 *   node tools/gen/shellcode_blob.mjs --in build/Debug/PHETAMINEStub.dll --out src/inject/blob/stub_blob.inc
 *   node tools/gen/shellcode_blob.mjs --self-test
 */

import { readFileSync, writeFileSync, mkdirSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = resolve(HERE, "..", "..");

export const MAX_BLOB_BYTES = 16 * 1024;
const IMAGE_REL_BASED_ABSOLUTE = 0;
const IMAGE_REL_BASED_DIR64 = 10;
const IMAGE_FILE_MACHINE_AMD64 = 0x8664;
const PE32_PLUS_MAGIC = 0x20b;

// ---------------------------------------------------------------- PE parse
export function parsePe(buf) {
  if (buf.length < 0x200) throw new Error("file too small to be a PE");
  if (buf.readUInt16LE(0) !== 0x5a4d) throw new Error("missing MZ signature");
  const peOff = buf.readUInt32LE(0x3c);
  if (buf.readUInt32LE(peOff) !== 0x00004550) throw new Error("missing PE\\0\\0 signature");

  const machine = buf.readUInt16LE(peOff + 4);
  const numSections = buf.readUInt16LE(peOff + 6);
  const optSize = buf.readUInt16LE(peOff + 20);
  const optOff = peOff + 24;
  if (buf.readUInt16LE(optOff) !== PE32_PLUS_MAGIC) throw new Error("not PE32+ (x64) — the stub must be x64");

  const entryRva = buf.readUInt32LE(optOff + 0x10);
  const dirOff = optOff + 0x70;
  const relocRva = buf.readUInt32LE(dirOff + 5 * 8);
  const relocSize = buf.readUInt32LE(dirOff + 5 * 8 + 4);

  const sections = [];
  const secOff = optOff + optSize;
  for (let i = 0; i < numSections; i++) {
    const o = secOff + i * 40;
    const name = buf.toString("ascii", o, o + 8).replace(/\0+$/, "");
    sections.push({
      name,
      virtualSize: buf.readUInt32LE(o + 8),
      virtualAddress: buf.readUInt32LE(o + 12),
      rawSize: buf.readUInt32LE(o + 16),
      rawPointer: buf.readUInt32LE(o + 20),
      characteristics: buf.readUInt32LE(o + 36),
    });
  }
  return { machine, entryRva, sections, relocRva, relocSize, buf };
}

function rvaToSection(pe, rva) {
  return pe.sections.find((s) => rva >= s.virtualAddress && rva < s.virtualAddress + Math.max(s.virtualSize, s.rawSize)) ?? null;
}

function parseRelocations(pe) {
  const out = [];
  if (!pe.relocRva || !pe.relocSize) return out;
  const sec = rvaToSection(pe, pe.relocRva);
  if (!sec) throw new Error(`relocation directory RVA 0x${pe.relocRva.toString(16)} is not inside any section`);
  let fileOff = sec.rawPointer + (pe.relocRva - sec.virtualAddress);
  const end = fileOff + pe.relocSize;
  while (fileOff < end) {
    const pageRva = pe.buf.readUInt32LE(fileOff);
    const blockSize = pe.buf.readUInt32LE(fileOff + 4);
    if (blockSize < 8) break;
    for (let e = fileOff + 8; e < fileOff + blockSize; e += 2) {
      const entry = pe.buf.readUInt16LE(e);
      const type = entry >> 12;
      const offset = entry & 0x0fff;
      if (type === IMAGE_REL_BASED_ABSOLUTE) continue; // padding
      out.push({ type, rva: pageRva + offset });
    }
    fileOff += blockSize;
  }
  return out;
}

/**
 * @returns {{ok: boolean, errors: string[], warnings: string[], blob?: Buffer, entryOffset?: number, textSection?: string}}
 */
export function extractStub(buf) {
  const errors = [];
  const warnings = [];
  let pe;
  try {
    pe = parsePe(buf);
  } catch (err) {
    return { ok: false, errors: [err.message], warnings };
  }

  if (pe.machine !== IMAGE_FILE_MACHINE_AMD64) {
    errors.push(`machine 0x${pe.machine.toString(16)} is not AMD64 — the client is x64`);
  }

  const text = pe.sections.find((s) => s.name === ".text");
  if (!text) return { ok: false, errors: [...errors, "no .text section"], warnings };

  const textEnd = text.virtualAddress + text.rawSize;
  if (pe.entryRva < text.virtualAddress || pe.entryRva >= textEnd) {
    errors.push(
      `entry RVA 0x${pe.entryRva.toString(16)} is outside .text ` +
      `[0x${text.virtualAddress.toString(16)}, 0x${textEnd.toString(16)}) — the injector jumps straight to it`);
  }

  let relocs = [];
  try {
    relocs = parseRelocations(pe);
  } catch (err) {
    errors.push(err.message);
  }
  const inText = relocs.filter((r) => r.rva >= text.virtualAddress && r.rva < textEnd);
  const outside = relocs.filter((r) => !(r.rva >= text.virtualAddress && r.rva < textEnd));
  if (inText.length) {
    errors.push(
      `${inText.length} relocation(s) target .text (first at RVA 0x${inText[0].rva.toString(16)}) — ` +
      `the stub is not position-independent; keep data in .text and address it RIP-relative`);
  }
  if (outside.length) {
    errors.push(
      `${outside.length} relocation(s) target non-.text addresses (first at RVA 0x${outside[0].rva.toString(16)}) — ` +
      `nothing applies them, so the blob would be wrong`);
  }

  let blob = Buffer.from(buf.subarray(text.rawPointer, text.rawPointer + text.rawSize));
  // trim alignment padding from the end (0xCC from the linker, or zeros)
  let end = blob.length;
  while (end > 0 && (blob[end - 1] === 0xcc || blob[end - 1] === 0x00)) end--;
  if (end === 0) errors.push(".text is empty after trimming padding");
  blob = blob.subarray(0, end);

  if (blob.length > MAX_BLOB_BYTES) {
    errors.push(`blob is ${blob.length} bytes > ${MAX_BLOB_BYTES} cap — trim the stub or raise the cap deliberately`);
  }
  if (blob.length && blob.length % 16 !== 0) {
    warnings.push(`blob is ${blob.length} bytes (not a multiple of 16) — fine, but unusual for a linked .text`);
  }
  if (errors.length) return { ok: false, errors, warnings };

  return {
    ok: true,
    errors,
    warnings,
    blob,
    entryOffset: pe.entryRva - text.virtualAddress,
    textSection: ".text",
  };
}

export function emitInc(result, meta = {}) {
  const bytes = [];
  for (let i = 0; i < result.blob.length; i++) bytes.push(result.blob[i]);
  const rows = [];
  for (let i = 0; i < bytes.length; i += 16) {
    rows.push("    " + bytes.slice(i, i + 16).map((b) => `0x${b.toString(16).padStart(2, "0")}`).join(", ") + ",");
  }
  return `// GENERATED by tools/gen/shellcode_blob.mjs — do not edit.
// source : ${meta.source ?? "(unknown)"}
// size   : ${result.blob.length} bytes, entry at 0x${result.entryOffset.toString(16)}
// checks : entry inside .text · zero relocations · size ≤ ${MAX_BLOB_BYTES}
#pragma once
#include <cstddef>

namespace phetamine::inject::blob {
    inline constexpr unsigned char kStub[] = {
${rows.join("\n")}
    };
    inline constexpr size_t kStubSize = ${result.blob.length};
    inline constexpr size_t kStubEntryOffset = 0x${result.entryOffset.toString(16)};
}
`;
}

// ---------------------------------------------------------------- self-test
/** Build a minimal synthetic PE32+ with the given relocations and entry RVA. */
function syntheticPe({ entryRva = 0x1000, textSize = 0x40, relocs = [] } = {}) {
  const secAlign = 0x1000, fileAlign = 0x200;
  const sections = [
    { name: ".text", va: 0x1000, vsize: textSize, rsize: align(textSize, fileAlign), chars: 0x60000020 },
    { name: ".rdata", va: 0x2000, vsize: 0x20, rsize: fileAlign, chars: 0x40000040 },
    { name: ".reloc", va: 0x3000, vsize: 0x40, rsize: fileAlign, chars: 0x42000040 },
  ];
  const optSize = 0xf0;
  const headersSize = align(0x18 + optSize + sections.length * 40, fileAlign);
  const total = headersSize + sections.reduce((a, s) => a + s.rsize, 0);
  const buf = Buffer.alloc(total);

  buf.writeUInt16LE(0x5a4d, 0);
  const peOff = 0x80;
  buf.writeUInt32LE(peOff, 0x3c);
  buf.writeUInt32LE(0x00004550, peOff);
  buf.writeUInt16LE(IMAGE_FILE_MACHINE_AMD64, peOff + 4);
  buf.writeUInt16LE(sections.length, peOff + 6);
  buf.writeUInt16LE(optSize, peOff + 20);

  const optOff = peOff + 24;
  buf.writeUInt16LE(PE32_PLUS_MAGIC, optOff);
  buf.writeUInt32LE(entryRva, optOff + 0x10);
  buf.writeUInt32LE(secAlign, optOff + 0x20);
  buf.writeUInt32LE(fileAlign, optOff + 0x24);
  buf.writeUInt32LE(headersSize, optOff + 0x3c);
  // data directories: [5] = base relocations
  const relocSectionIndex = sections.findIndex((s) => s.name === ".reloc");
  const relocSec = sections[relocSectionIndex];
  if (relocs.length) {
    buf.writeUInt32LE(relocSec.va, optOff + 0x70 + 5 * 8);
    buf.writeUInt32LE(8 + relocs.length * 2, optOff + 0x70 + 5 * 8 + 4);
  }

  const secOff = optOff + optSize;
  let rawPointer = headersSize;
  sections.forEach((s, i) => {
    const o = secOff + i * 40;
    buf.write(s.name.padEnd(8, "\0"), o, 8, "ascii");
    buf.writeUInt32LE(s.vsize, o + 8);
    buf.writeUInt32LE(s.va, o + 12);
    buf.writeUInt32LE(s.rsize, o + 16);
    buf.writeUInt32LE(rawPointer, o + 20);
    buf.writeUInt32LE(s.chars, o + 36);
    s.rawPointer = rawPointer;
    rawPointer += s.rsize;
  });

  // fill .text with a plausible looking prologue
  const text = sections[0];
  for (let i = 0; i < text.vsize; i++) buf[text.rawPointer + i] = 0x90;
  buf[text.rawPointer] = 0x48; buf[text.rawPointer + 1] = 0x83; buf[text.rawPointer + 2] = 0xec;
  // tail padding the trimmer should remove
  buf[text.rawPointer + text.vsize - 1] = 0xcc;

  if (relocs.length) {
    const ro = relocSec.rawPointer;
    buf.writeUInt32LE(relocs[0].page, ro);
    buf.writeUInt32LE(8 + relocs.length * 2, ro + 4);
    relocs.forEach((r, i) => {
      buf.writeUInt16LE((r.type << 12) | r.offset, ro + 8 + i * 2);
    });
  }
  return buf;
}

const align = (n, a) => Math.ceil(n / a) * a;

export function selfTest() {
  const results = [];
  const check = (name, cond, detail) => results.push({ name, pass: !!cond, detail });

  const clean = syntheticPe();
  const cleanRes = extractStub(clean);
  check("accepts a reloc-free .text",
    cleanRes.ok && cleanRes.blob.length === 0x3f && cleanRes.entryOffset === 0,
    `ok=${cleanRes.ok} size=${cleanRes.blob?.length} entry=0x${cleanRes.entryOffset?.toString(16)} errors=${cleanRes.errors.join("; ")}`);

  const relocInside = syntheticPe({ relocs: [{ type: IMAGE_REL_BASED_DIR64, page: 0x1000, offset: 0x8 }] });
  const insideRes = extractStub(relocInside);
  check("rejects a relocation inside .text",
    !insideRes.ok && insideRes.errors.some((e) => e.includes("not position-independent")),
    insideRes.errors[0] ?? "(no error raised)");

  const relocOutside = syntheticPe({ relocs: [{ type: IMAGE_REL_BASED_DIR64, page: 0x2000, offset: 0x10 }] });
  const outsideRes = extractStub(relocOutside);
  check("rejects a relocation outside .text",
    !outsideRes.ok && outsideRes.errors.some((e) => e.includes("nothing applies them")),
    outsideRes.errors[0] ?? "(no error raised)");

  const badEntry = syntheticPe({ entryRva: 0x2000 });
  const badEntryRes = extractStub(badEntry);
  check("rejects an entry outside .text",
    !badEntryRes.ok && badEntryRes.errors.some((e) => e.includes("outside .text")),
    badEntryRes.errors[0] ?? "(no error raised)");

  const notPe = Buffer.alloc(512);
  const notPeRes = extractStub(notPe);
  check("rejects a non-PE buffer", !notPeRes.ok, notPeRes.errors[0]);

  const inc = emitInc(cleanRes, { source: "synthetic" });
  check("emits a compilable .inc shape",
    inc.includes("inline constexpr unsigned char kStub[]") && /kStubSize = \d+;/.test(inc) && inc.includes("kStubEntryOffset"),
    `${inc.split("\n").length} lines`);

  return results;
}

// ---------------------------------------------------------------- CLI
function main(argv) {
  const arg = (flag, dflt) => {
    const i = argv.indexOf(flag);
    return i === -1 ? dflt : argv[i + 1];
  };
  if (argv.includes("--self-test")) {
    const results = selfTest();
    for (const r of results) console.log(`${r.pass ? "PASS" : "FAIL"} · ${r.name} · ${r.detail}`);
    return results.every((r) => r.pass) ? 0 : 2;
  }
  const inPath = arg("--in");
  const outPath = arg("--out", resolve(ROOT, "src/inject/blob/stub_blob.inc"));
  if (!inPath) {
    console.error("usage: shellcode_blob.mjs --in <linked-stub-pe> [--out <stub_blob.inc>] | --self-test");
    return 2;
  }
  const res = extractStub(readFileSync(inPath));
  for (const w of res.warnings) console.error(`warning: ${w}`);
  if (!res.ok) {
    console.error(`stub blob REJECTED (${res.errors.length} error(s)):`);
    for (const e of res.errors) console.error(`  - ${e}`);
    return 1;
  }
  mkdirSync(dirname(outPath), { recursive: true });
  writeFileSync(outPath, emitInc(res, { source: inPath }));
  console.error(`wrote ${outPath} — ${res.blob.length} bytes, entry +0x${res.entryOffset.toString(16)}`);
  return 0;
}

if (import.meta.url === `file://${process.argv[1]}`) {
  process.exitCode = main(process.argv.slice(2));
}
