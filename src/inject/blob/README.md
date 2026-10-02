# The loader blob

`stub.c` in the parent directory is the source of the loader that the UI copies
into the client. It is **not** linked into `PHETAMINE.dll` and it is **not**
loaded by the OS — it is turned into a raw byte blob and started as a thread:

```
  CMake                       tools/gen/shellcode_blob.mjs
  ─────                       ────────────────────────────
  PHETAMINEStub.dll    ──▶    PHETAMINE.stub.bin   ──▶    embedded / shipped
  (.text only, reloc-free)    (raw shellcode + entry offset)
```

The generator enforces the contract; this file records it, so that a change to
either side has a place to be checked against. `node tools/selftest.mjs` runs the
generator's own tests.

## Contract between the three sides

| | provides | must be true |
|---|---|---|
| **CMake** (`PHETAMINEStub`) | a linked PE whose `.text` is the loader | `AddressOfEntryPoint` inside `.text`; **zero** relocations anywhere that targets code; all string data merged into `.text` (`/MERGE:.rdata=.text`) |
| **Generator** (`shellcode_blob.mjs`) | `kStub`, `kStubSize`, `kStubEntryOffset` | blob ≤ 16 KiB; trailing `0xCC`/`0x00` trimmed; offsets are relative to the blob start |
| **Injector** (`ui/PHETAMINEUI/Injector.cs`) | a target-process thread at `stubBase + kStubEntryOffset`, with `lpParameter` = a `StubPayload` | see below |
| **Loader** (`stub.c`) | a mapped `PHETAMINE.dll`, then a call to `PhetamineEntry` | returns 0 on success; every non-zero exit code is documented in the source and surfaced by the UI |

## What the injector writes into the target

```
  imageBase      ← VirtualAllocEx(preferredBase, SizeOfImage) + the mapped image bytes
  stubBase       ← VirtualAllocEx(NULL, blobSize + 0x1000), blob written, then RX
  paramBase      ← a ParamBlock (src/inject/param.h), 816 bytes
  payloadBase    ← StubPayload:

      struct StubPayload {
          void* image_base;    // imageBase
          u32   image_size;    // SizeOfImage
          u32   entry_rva;     // 0 ⇒ the stub parses the export directory for PhetamineEntry
          void* param_block;   // paramBase
      };

  CreateRemoteThread(image = stubBase + kStubEntryOffset, parameter = payloadBase)
```

The module image is copied in **already laid out as if mapped**: headers at
offset 0, each section at its virtual address, zero-filled to the virtual size
(`PeImage.BuildMappedImage`). That is why the stub does not need to know where the
file was read from, and why the injector refuses an image with base relocations:
mapping at the preferred base is the whole reason the layout is that simple.

## Deliberate omissions (ADR-7)

The loader does not: run TLS callbacks, rewrite the exception directory, erase
headers, register an exception handler, or queue APCs. Each of those is either an
anti-forensics measure (out of scope by policy — removal makes the loader worse at
its actual job) or a correctness risk (exception-directory rewriting is the
classic "crashes an hour later" bug in hand-rolled loaders).

## Verifying a build

```powershell
# 1. does the linked module satisfy what the stub needs?
node tools/gen/check_module.mjs --module build/PHETAMINE.dll --expect-version version-02c37bc51a384b8f

# 2. is the blob still a position-independent .text with a sane entry?
node tools/gen/shellcode_blob.mjs --in build/PHETAMINEStub.dll --out build/PHETAMINE.stub.bin
```

`check_module.mjs` failing is a **build** failure: fix the link flags (it prints
which property is wrong), do not "fix" the loader.
