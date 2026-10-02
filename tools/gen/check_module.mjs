#!/usr/bin/env node
/**
 * tools/gen/check_module.mjs — validate the LINKED module image.
 *
 * The shellcode stub maps PHETAMINE.dll at its preferred base and fixes it up by
 * hand. That only works if the linked image actually has the properties ADR-1
 * claims, and those properties are decided by the LINKER, not by the source. So
 * they are read back out of the built file:
 *
 *   1. PE32+ / AMD64 / DLL — a 32-bit or EXE image cannot be mapped by the stub;
 *   2. DataDirectory[BASERELOC] is empty — a relocatable image would need fix-ups
 *      the stub deliberately does not perform;
 *   3. the image exports `PhetamineEntry` — the stub looks it up by name, and a
 *      missing export is exit code 9 at injection time (`src/inject/stub/stub.c`);
 *   4. (informational) the expected Roblox version string is present, so a stale
 *      generated offset table is visible in the build log rather than at runtime.
 *
 * Usage:
 *   node tools/gen/check_module.mjs --module build/PHETAMINE.dll --expect-version version-...
 *   node tools/gen/check_module.mjs --self-test
 */
import { readFileSync, existsSync } from 'node:fs';

const PAGE = 0x1000;

function parsePe(buf) {
  if (buf.length < 0x100) throw new Error('file is smaller than a DOS header');
  if (buf.readUInt16LE(0) !== 0x5a4d) throw new Error('no MZ signature');
  const peOff = buf.readUInt32LE(0x3c);
  if (peOff + 0x108 > buf.length) throw new Error('PE header offset is out of range');
  if (buf.readUInt32LE(peOff) !== 0x00004550) throw new Error('no PE signature');

  const machine = buf.readUInt16LE(peOff + 4);
  const numSections = buf.readUInt16LE(peOff + 6);
  const optSize = buf.readUInt16LE(peOff + 20);
  const characteristics = buf.readUInt16LE(peOff + 22);
  const opt = peOff + 24;
  const magic = buf.readUInt16LE(opt);
  const isPe32Plus = magic === 0x20b;

  const entryRva = isPe32Plus ? buf.readUInt32LE(opt + 0x10) : buf.readUInt32LE(opt + 0x10);
  const sizeOfImage = buf.readUInt32LE(opt + 0x38);
  const numDirectories = buf.readUInt32LE(opt + (isPe32Plus ? 0x6c : 0x5c));
  const dirBase = opt + (isPe32Plus ? 0x70 : 0x60);
  const directories = [];
  for (let i = 0; i < Math.min(numDirectories, 16); i++) {
    directories.push({
      rva: buf.readUInt32LE(dirBase + i * 8),
      size: buf.readUInt32LE(dirBase + i * 8 + 4),
    });
  }

  const sections = [];
  const sectionTable = opt + optSize;
  for (let i = 0; i < numSections; i++) {
    const at = sectionTable + i * 40;
    const name = buf.toString('ascii', at, at + 8).replace(/\0+$/, '');
    sections.push({
      name,
      virtualSize: buf.readUInt32LE(at + 8),
      virtualAddress: buf.readUInt32LE(at + 12),
      rawSize: buf.readUInt32LE(at + 16),
      rawPointer: buf.readUInt32LE(at + 20),
    });
  }

  return {
    buf, machine, numSections, characteristics, isPe32Plus, entryRva, sizeOfImage,
    directories, sections,
    isDll: (characteristics & 0x2000) !== 0,
  };
}

function rvaToOffset(pe, rva) {
  for (const section of pe.sections) {
    const span = Math.max(section.virtualSize, section.rawSize);
    if (rva >= section.virtualAddress && rva < section.virtualAddress + span) {
      return section.rawPointer + (rva - section.virtualAddress);
    }
  }
  return rva < pe.sections[0]?.rawPointer ? rva : -1;
}

/** Reads the export names from DataDirectory[0]. */
function exportNames(pe) {
  const directory = pe.directories[0];
  if (!directory || !directory.rva || !directory.size) return [];
  const directoryOffset = rvaToOffset(pe, directory.rva);
  if (directoryOffset < 0) return [];
  const numberOfNames = pe.buf.readUInt32LE(directoryOffset + 24);
  const namesRva = pe.buf.readUInt32LE(directoryOffset + 32);
  const namesOffset = rvaToOffset(pe, namesRva);
  if (namesOffset < 0) return [];
  const names = [];
  for (let i = 0; i < numberOfNames; i++) {
    const nameRva = pe.buf.readUInt32LE(namesOffset + i * 4);
    const offset = rvaToOffset(pe, nameRva);
    if (offset < 0) continue;
    let end = offset;
    while (end < pe.buf.length && pe.buf[end] !== 0) end++;
    names.push(pe.buf.toString('ascii', offset, end));
  }
  return names;
}

export function checkModule(buffer, { expectVersion = null } = {}) {
  const failures = [];
  const notes = [];

  let pe;
  try {
    pe = parsePe(buffer);
  } catch (error) {
    return { ok: false, failures: [`not a usable PE image: ${error.message}`], notes: [] };
  }

  if (!pe.isPe32Plus) failures.push('not PE32+ (a 32-bit image cannot be mapped by the stub)');
  if (pe.machine !== 0x8664) failures.push(`machine is 0x${pe.machine.toString(16)}, not AMD64 (0x8664)`);
  if (!pe.isDll) failures.push('the image is not a DLL (CHARACTERISTICS does not set IMAGE_FILE_DLL)');
  if (pe.entryRva === 0) failures.push('AddressOfEntryPoint is 0 — nothing would run at DllMain');

  const reloc = pe.directories[5];
  if (reloc && (reloc.rva !== 0 || reloc.size !== 0)) {
    failures.push(
      `DataDirectory[BASERELOC] is present (rva 0x${reloc.rva.toString(16)}, size 0x${reloc.size.toString(16)}): ` +
      'the image is relocatable. Link with /FIXED /DYNAMICBASE:NO so the preferred-base map is deterministic.');
  } else {
    notes.push('no base relocations');
  }

  const names = exportNames(pe);
  if (!names.includes('PhetamineEntry')) {
    failures.push(
      `the export \`PhetamineEntry\` is missing (exports found: ${names.length ? names.join(', ') : 'none'}). ` +
      'The stub resolves it by name; without it, injection fails with exit code 9.');
  } else {
    notes.push('PhetamineEntry exported');
  }

  if (expectVersion) {
    const text = buffer.toString('latin1');
    if (!text.includes(expectVersion)) {
      notes.push(`the expected version string "${expectVersion}" was not found in the image — ` +
        'the generated offset table may be stale (not a hard failure: the DLL fails closed per feature)');
    } else {
      notes.push(`version string ${expectVersion} present`);
    }
  }

  notes.push(`${pe.sections.length} sections, size of image 0x${pe.sizeOfImage.toString(16)}`);
  return { ok: failures.length === 0, failures, notes };
}

// ------------------------------------------------------------------ self-test
function align(value, to) { return (value + to - 1) & ~(to - 1); }

/** Builds a minimal but structurally valid PE for the self-test. */
function syntheticPe({ machine = 0x8664, dll = true, reloc = false, exportEntry = true, pe32plus = true } = {}) {
  const sectionAlignment = 0x1000;
  const fileAlignment = 0x200;
  const sections = [];

  const makeSection = (name, virtualSize, rawSize, characteristics, fill) => {
    const section = { name, virtualSize, rawSize, characteristics, fill };
    sections.push(section);
    return section;
  };

  const text = makeSection('.text', 0x40, 0x200, 0x60000020, 0xcc);
  const edata = makeSection('.edata', 0x80, 0x200, 0x40000040, 0x00);
  if (reloc) makeSection('.reloc', 0x20, 0x200, 0x42000040, 0x00);

  let rva = sectionAlignment;
  for (const section of sections) {
    section.virtualAddress = rva;
    rva += align(section.virtualSize, sectionAlignment);
  }
  const sizeOfImage = rva;

  const headersSize = align(0x18 + 0x70 + sections.length * 40, fileAlignment);
  let fileSize = headersSize;
  for (const section of sections) {
    section.rawPointer = fileSize;
    fileSize += align(section.rawSize, fileAlignment);
  }

  const buf = Buffer.alloc(fileSize, 0);
  buf.writeUInt16LE(0x5a4d, 0);
  buf.writeUInt32LE(0x80, 0x3c);
  const peOff = 0x80;
  buf.writeUInt32LE(0x00004550, peOff);
  buf.writeUInt16LE(machine, peOff + 4);
  buf.writeUInt16LE(sections.length, peOff + 6);
  buf.writeUInt16LE(0xE0, peOff + 20);
  buf.writeUInt16LE((dll ? 0x2000 : 0) | 0x0022, peOff + 22);   // EXECUTABLE_IMAGE | LARGE_ADDRESS_AWARE

  const opt = peOff + 24;
  buf.writeUInt16LE(pe32plus ? 0x20b : 0x10b, opt);
  buf.writeUInt32LE(0x1000, opt + 0x10);                        // AddressOfEntryPoint
  buf.writeBigUInt64LE(0x180000000n, opt + 24);
  buf.writeUInt32LE(sectionAlignment, opt + 32);
  buf.writeUInt32LE(fileAlignment, opt + 36);
  buf.writeUInt32LE(sizeOfImage, opt + 56);
  buf.writeUInt32LE(headersSize, opt + 60);
  buf.writeUInt32LE(16, opt + 0x6c);                            // NumberOfRvaAndSizes
  const dirBase = opt + 0x70;
  if (exportEntry) {
    buf.writeUInt32LE(edata.virtualAddress, dirBase + 0 * 8);
    buf.writeUInt32LE(0x40, dirBase + 0 * 8 + 4);
  }
  if (reloc) {
    const relocSection = sections.find((s) => s.name === '.reloc');
    buf.writeUInt32LE(relocSection.virtualAddress, dirBase + 5 * 8);
    buf.writeUInt32LE(0x20, dirBase + 5 * 8 + 4);
  }

  const sectionTable = opt + 0xE0;
  sections.forEach((section, index) => {
    const at = sectionTable + index * 40;
    buf.write(section.name, at, 'ascii');
    buf.writeUInt32LE(section.virtualSize, at + 8);
    buf.writeUInt32LE(section.virtualAddress, at + 12);
    buf.writeUInt32LE(section.rawSize, at + 16);
    buf.writeUInt32LE(section.rawPointer, at + 20);
    buf.writeUInt32LE(section.characteristics, at + 36);
    buf.fill(section.fill, section.rawPointer, section.rawPointer + section.rawSize);
  });

  if (exportEntry) {
    // One export: PhetamineEntry (name table + one function + one name pointer).
    // IMAGE_EXPORT_DIRECTORY: +16 Base, +20 NumberOfFunctions, +24 NumberOfNames,
    // +28 AddressOfFunctions, +32 AddressOfNames, +36 AddressOfNameOrdinals
    // (the three "Address*" fields are RVAs relative to the section they live in).
    const base = edata.rawPointer;
    buf.writeUInt32LE(0, base + 12);                             // NameRVA (unused)
    buf.writeUInt32LE(1, base + 16);                             // Base
    buf.writeUInt32LE(1, base + 20);                             // NumberOfFunctions
    buf.writeUInt32LE(1, base + 24);                             // NumberOfNames
    buf.writeUInt32LE(edata.virtualAddress + 0x28, base + 28);   // AddressOfFunctions
    buf.writeUInt32LE(edata.virtualAddress + 0x30, base + 32);   // AddressOfNames
    buf.writeUInt32LE(edata.virtualAddress + 0x38, base + 36);   // AddressOfNameOrdinals
    buf.writeUInt32LE(0x1000, base + 0x28);                      // function RVA
    buf.writeUInt32LE(edata.virtualAddress + 0x40, base + 0x30); // name RVA
    buf.writeUInt16LE(0, base + 0x38);                           // ordinal
    buf.write('PhetamineEntry\0', base + 0x40, 'ascii');
  }

  // a version string in .text, as a real module would carry
  buf.write('version-02c37bc51a384b8f\0', text.rawPointer + 0x20, 'latin1');
  return buf;
}

function selfTest() {
  const cases = [
    ['accepts a well-formed module', syntheticPe({}), true],
    ['rejects a relocatable module', syntheticPe({ reloc: true }), false],
    ['rejects a missing entry export', syntheticPe({ exportEntry: false }), false],
    ['rejects a 32-bit module', syntheticPe({ pe32plus: false }), false],
    ['rejects an EXE', syntheticPe({ dll: false }), false],
  ];
  let failed = 0;
  for (const [name, buffer, expected] of cases) {
    const result = checkModule(buffer, { expectVersion: 'version-02c37bc51a384b8f' });
    const good = result.ok === expected;
    if (!good) failed++;
    console.log(`${good ? 'PASS' : 'FAIL'} · ${name} · ok=${result.ok}${result.failures.length ? ` (${result.failures[0]})` : ''}`);
  }
  return failed;
}

function main(argv) {
  if (argv.includes('--self-test')) return selfTest() ? 1 : 0;

  const arg = (name) => {
    const index = argv.indexOf(name);
    return index >= 0 ? argv[index + 1] : null;
  };
  const path = arg('--module');
  if (!path) {
    console.error('usage: node tools/gen/check_module.mjs --module <PHETAMINE.dll> [--expect-version <ver>]');
    console.error('       node tools/gen/check_module.mjs --self-test');
    return 2;
  }
  if (!existsSync(path)) {
    console.error(`check_module: ${path} does not exist`);
    return 2;
  }

  const result = checkModule(readFileSync(path), { expectVersion: arg('--expect-version') });
  for (const note of result.notes) console.log(`  · ${note}`);
  for (const failure of result.failures) console.log(`  ✗ ${failure}`);
  if (!result.ok) {
    console.error(`check_module: ${path} does not satisfy the loader contract (docs/DECISIONS.md ADR-1)`);
    return 1;
  }
  console.log(`check_module: ${path} is mappable by the stub`);
  return 0;
}

if (process.argv[1] && process.argv[1].endsWith('check_module.mjs')) {
  process.exitCode = main(process.argv.slice(2));
}
