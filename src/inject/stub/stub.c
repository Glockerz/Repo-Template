// inject/stub/stub.c — the shellcode resident loader.
//
// This is NOT compiled into PHETAMINE.dll. It is built as a freestanding,
// position-independent x64 blob and embedded into the injector:
//
//     clang --target=x86_64-pc-windows-msvc -O2 -fno-stack-protector -nostdlib \
//           -fno-builtin -ffreestanding -fno-asynchronous-unwind-tables \
//           -Wl,--no-insert-timestamp -o stub.bin stub.c        (see blob/README.md)
//
// The injector writes the blob plus a `ParamBlock` (inject/param.h) into the
// target, then starts a thread at the blob's entry point with the param block as
// its argument. What the blob does, in order:
//
//   1. resolve the handful of ntdll routines it needs by walking the PEB's
//      loader lists (no GetProcAddress, no import table);
//   2. allocate the image at its preferred base, map each section, apply
//      relocations (the blob itself must not need any);
//   3. resolve the mapped image's imports, then protect sections with the rights
//      from the section headers;
//   4. parse the image's export directory for `PhetamineEntry` and call it with
//      the param block;
//   5. return and let the injector clean up the blob.
//
// Deliberate omissions, because they are the parts that turn a loader into a
// detection engine and they buy nothing here (docs/DECISIONS.md ADR-7):
//   * no TLS callbacks (the client image we host does not rely on them across a
//     manual map, and running them before DllMain is a common crash source);
//   * no exception-directory rewriting — the mapped image registers its own
//     handlers through the normal CRT/API paths;
//   * no .reloc-free "erase the headers" pass (anti-forensics is out of scope);
//   * no APC queuing — the loader runs on its own thread; the APC path is in the
//     DLL's rendezvous code and stays off by default.
//
// The blob must stay under 16 KiB (tools/gen/shellcode_blob.mjs enforces it) and
// must NOT contain the `MZ` bytes at offset 0 — it starts with shellcode, and the
// generator checks for that.

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef long long          i64;

#define PHETAMINE_STUB_EXPORT "PhetamineEntry"

// ---------------------------------------------------------------- primitives

static int stub_wcslen(const wchar_t* text) {
    int length = 0;
    while (text && text[length]) ++length;
    return length;
}

static int stub_streq(const char* a, const char* b) {
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}

// ---------------------------------------------------------------- PEB walk
//
// The target may not have loaded anything we can call by name, so everything
// comes from the PEB: process parameters → InMemoryOrderModuleList → ntdll, then
// ntdll's own export table for LdrLoadDll / LdrGetProcedureAddress.

typedef struct {
    u16 length;
    u16 maximumLength;
    wchar_t* buffer;
} StubUnicodeString;

typedef struct {
    u32 length;
    u32 initialised;
    u64 ssHandle;
    void* in_load_order;
    void* in_memory_order;
    void* in_initialisation_order;
    void* entry_point;
} StubLdrEntryBase;

typedef struct {
    StubLdrEntryBase base;
    void* dll_base;
    void* entry_point;
    u32 size_of_image;
    u32 flags;
    u16 full_dll_name_length;
    u16 full_dll_name_maximum_length;
    u32 load_count;
    StubUnicodeString full_dll_name;
} StubLdrEntry;

typedef struct {
    u32 length;
    u8 initialised;
    u64 ss;
    void* list;   // LIST_ENTRY of module entries
} StubPebLdr;

// The PEB only matters for `.Ldr` (offset 0x18 on x64) and, through it, the
// loader's module lists. Everything else in the structure is deliberately not
// modelled: a field-for-field PEB replica is a maintenance trap that buys nothing.
typedef struct {
    u8 reserved[0x18];
    StubPebLdr* ldr;
} StubPeb;

static StubPeb* stub_peb(void) {
#if defined(_MSC_VER)
    extern unsigned long long __readgsqword(unsigned long);
    return (StubPeb*)__readgsqword(0x60);
#else
    StubPeb* peb = 0;
    __asm__ __volatile__("movq %%gs:0x60, %0" : "=r"(peb));
    return peb;
#endif
}

// Walk the in-memory-order list and return the base of `name` (ANSI compare,
// case-insensitive only for the letters that matter — ntdll/kernel32). A full
// case-insensitive compare costs code we do not need.
static void* stub_find_module(StubPeb* peb, const wchar_t* name, int caseInsensitive) {
    if (!peb || !peb->ldr || !peb->ldr->list) return 0;
    void* head = peb->ldr->list;
    void* walk = *(void**)head;     // first LIST_ENTRY
    for (int guard = 0; walk != head && guard < 512; ++guard) {
        // LDR_DATA_TABLE_ENTRY.InMemoryOrderLinks is at offset 0x10 on x64; the
        // entry base is therefore (link - 0x10).
        StubLdrEntry* entry = (StubLdrEntry*)((u8*)walk - 0x10);
        walk = *(void**)walk;
        if (!entry->dll_base) continue;
        const wchar_t* moduleName = entry->base.base.full_dll_name.buffer;
        if (!moduleName) continue;

        int index = 0;
        for (;;) {
            wchar_t a = moduleName[index];
            wchar_t b = name[index];
            if (caseInsensitive) {
                if (a >= L'A' && a <= L'Z') a = (wchar_t)(a + 32);
                if (b >= L'A' && b <= L'Z') b = (wchar_t)(b + 32);
            }
            if (!a || a != b) break;
            ++index;
        }
        if (moduleName[index] == 0 && name[index] == 0) return entry->dll_base;
        if (guard > 400) break;
    }
    return 0;
}

// ---------------------------------------------------------------- exports

typedef struct {
    u32 characteristics;
    u32 timestamp;
    u32 major;
    u32 minor;
    u32 name_rva;
    u32 base;
    u32 number_of_functions;
    u32 number_of_names;
    u32 address_of_functions;
    u32 address_of_names;
    u32 address_of_name_ordinals;
} StubExportDirectory;

static u32 stub_rva_to_offset(u32 rva) { return rva; }   // sections are mapped at their RVAs

static void* stub_get_proc(void* moduleBase, const char* name) {
    if (!moduleBase) return 0;
    u8* base = (u8*)moduleBase;
    // DOS → NT headers
    if (*(u16*)base != 0x5A4D) return 0;                       // "MZ"
    u32 ntOffset = *(u32*)(base + 0x3C);
    u8* nt = base + ntOffset;
    if (*(u32*)nt != 0x00004550) return 0;                     // "PE\0\0"
    u32 exportRva = *(u32*)(nt + 0x18 + 0x70);
    u32 exportSize = *(u32*)(nt + 0x18 + 0x74);
    if (!exportRva || !exportSize) return 0;

    StubExportDirectory* exports = (StubExportDirectory*)(base + stub_rva_to_offset(exportRva));
    const char** names = (const char**)(base + stub_rva_to_offset(exports->address_of_names));
    u16* ordinals = (u16*)(base + stub_rva_to_offset(exports->address_of_name_ordinals));
    u32* functions = (u32*)(base + stub_rva_to_offset(exports->address_of_functions));

    for (u32 i = 0; i < exports->number_of_names; ++i) {
        if (stub_streq(names[i], name)) {
            u32 functionRva = functions[ordinals[i]];
            return base + stub_rva_to_offset(functionRva);
        }
    }
    return 0;
}

// ---------------------------------------------------------------- imports

typedef struct {
    u32 original_first_thunk;
    u32 timestamp;
    u32 forwarder_chain;
    u32 name;
    u32 first_thunk;
} StubImportDescriptor;

// Resolve the mapped image's imports with LdrGetProcedureAddress (falling back to
// LdrLoadDll for modules that are not resident). Returns the number of failures.
static u32 stub_resolve_imports(
    u8* image,
    void* (*ldr_load_dll)(StubUnicodeString*, u32, void*, void**),
    int (*ldr_get_procedure)(void*, void*, u32, void**)) {

    u8* base = image;
    u32 ntOffset = *(u32*)(base + 0x3C);
    u8* nt = base + ntOffset;
    const u32 importRva = *(u32*)(nt + 0x18 + 0x90);   // DataDirectory[IMPORT]
    if (!importRva) return 0;

    u32 failures = 0;
    StubImportDescriptor* descriptor =
        (StubImportDescriptor*)(base + stub_rva_to_offset(importRva));

    for (; descriptor->name; ++descriptor) {
        const char* moduleName = (const char*)(base + descriptor->name);
        void* module = 0;

        // Build a UNICODE_STRING without an allocator: write the buffer into a
        // stack array (module names are short).
        StubUnicodeString wide{};
        wchar_t buffer[64];
        int length = 0;
        while (moduleName[length] && length < 63) {
            buffer[length] = (wchar_t)(u8)moduleName[length];
            ++length;
        }
        buffer[length] = 0;
        wide.buffer = buffer;
        wide.length = (u16)(length * 2);
        wide.maximumLength = (u16)((length + 1) * 2);

        // Try the loader list first (the client has loaded everything we need),
        // then ask LdrLoadDll.
        StubPeb* peb = stub_peb();
        module = stub_find_module(peb, buffer, 1);
        if (!module && ldr_load_dll) {
            void* handle = 0;
            ldr_load_dll(&wide, 0, 0, &handle);
            module = handle;
        }
        if (!module) { ++failures; continue; }

        u32 thunkRva = descriptor->original_first_thunk ? descriptor->original_first_thunk
                                                        : descriptor->first_thunk;
        u64* thunk = (u64*)(base + thunk_rva);
        void** iat = (void**)(base + descriptor->first_thunk);
        for (; *thunk; ++thunk, ++iat) {
            if (*thunk & 0x8000000000000000ull) {
                // import by ordinal
                u32 ordinal = (u32)(*thunk & 0xFFFF);
                void* address = 0;
                if (ldr_get_procedure) ldr_get_procedure(module, 0, ordinal, &address);
                if (!address) ++failures;
                *iat = address;
            } else {
                const char* name = (const char*)(base + (u32)(*thunk & 0x7FFFFFFF) + 2);
                void* address = stub_get_proc(module, name);
                if (!address && ldr_get_procedure) ldr_get_procedure(module, (void*)name, 0, &address);
                if (!address) ++failures;
                *iat = address;
            }
        }
    }
    return failures;
}

// ---------------------------------------------------------------- mapping

typedef struct {
    u32 virtual_size;
    u32 virtual_address;
    u32 size_of_raw_data;
    u32 pointer_to_raw_data;
} StubSectionHeader;

typedef struct {
    u8* loader_base;      // this blob's mapping (unused afterwards, kept for the DLL)
    u8* image_base;       // the mapped PHETAMINE.dll
    u32 image_size;
} StubResult;

static void stub_apply_relocations(u8* image, i64 delta) {
    if (delta == 0) return;
    u8* base = image;
    u32 ntOffset = *(u32*)(base + 0x3C);
    u8* nt = base + ntOffset;
    const u32 relocRva = *(u32*)(nt + 0x18 + 0xA0);   // DataDirectory[BASERELOC]
    const u32 relocSize = *(u32*)(nt + 0x18 + 0xA4);
    if (!relocRva || !relocSize) return;

    u8* cursor = base + relocRva;
    u8* end = cursor + relocSize;
    while (cursor < end) {
        u32 pageRva = *(u32*)cursor;
        u32 blockSize = *(u32*)(cursor + 4);
        if (!blockSize) break;
        u16* entries = (u16*)(cursor + 8);
        const u32 count = (blockSize - 8) / 2;
        for (u32 i = 0; i < count; ++i) {
            const u16 type = (u16)(entries[i] >> 12);
            const u16 offset = (u16)(entries[i] & 0xFFF);
            u8* target = base + pageRva + offset;
            if (type == 3) {                                   // IMAGE_REL_BASED_HIGHLOW
                *(u32*)target = (u32)(*(u32*)target + (u32)delta);
            } else if (type == 10) {                           // IMAGE_REL_BASED_DIR64
                *(u64*)target = (u64)(*(u64*)target + (u64)delta);
            }
        }
        cursor += blockSize;
    }
}

// The blob's entry point. The injector starts a thread here with `params` in RCX.
__declspec(dllexport) int __stdcall StubMain(void* params) {
    StubPeb* peb = stub_peb();
    if (!peb || !peb->ldr) return 1;

    // 1. ntdll, then the three routines the rest of this file needs.
    void* ntdll = stub_find_module(peb, L"ntdll.dll", 1);
    if (!ntdll) return 2;

    typedef void* (*LdrLoadDllFn)(StubUnicodeString*, u32, void*, void**);
    typedef int (*LdrGetProcedureAddressFn)(void*, void*, u32, void**);
    typedef void* (*VirtualAllocFn)(void*, u64, u32, u32);
    typedef int (*VirtualProtectFn)(void*, u64, u32, u32*);

    LdrLoadDllFn ldr_load = (LdrLoadDllFn)stub_get_proc(ntdll, "LdrLoadDll");
    LdrGetProcedureAddressFn ldr_get = (LdrGetProcedureAddressFn)stub_get_proc(
        ntdll, "LdrGetProcedureAddress");
    if (!ldr_load || !ldr_get) return 3;

    void* kernel32 = 0;
    {
        StubUnicodeString name{};
        wchar_t buffer[] = L"kernel32.dll";
        name.buffer = buffer;
        name.length = (u16)(stub_wcslen(buffer) * 2);
        name.maximumLength = name.length + 2;
        void* handle = 0;
        ldr_load(&name, 0, 0, &handle);
        kernel32 = handle;
    }
    if (!kernel32) return 4;

    VirtualAllocFn stub_virtual_alloc = 0;
    VirtualProtectFn stub_virtual_protect = 0;
    ldr_get(kernel32, (void*)"VirtualAlloc", 0, (void**)&stub_virtual_alloc);
    ldr_get(kernel32, (void*)"VirtualProtect", 0, (void**)&stub_virtual_protect);
    if (!stub_virtual_alloc || !stub_virtual_protect) return 5;

    // 2. Where is the image we are supposed to map? The injector passes the image
    //    bytes through the param block (payloadSize/payload at the end of the
    //    block) OR, more commonly, has already written them at `imageBase` with
    //    MEM_RESERVE|MEM_COMMIT and asks us only to fix them up. Both modes are
    //    expressed by the flags; this blob implements the second (simpler, and it
    //    keeps the blob free of payload-size assumptions).
    //
    //    The injector contract (see blob/README.md):
    //      * it allocates the image at the preferred base and copies the headers
    //        and sections itself;
    //      * it passes a pointer to a `StubPayload` in `params`, which carries the
    //        image base, the entry RVA of the export and the param block.
    typedef struct {
        u8* image_base;
        u32 image_size;
        u32 entry_rva;         // 0 ⇒ parse the export directory for PhetamineEntry
        void* param_block;
    } StubPayload;

    StubPayload* payload = (StubPayload*)params;
    if (!payload || !payload->image_base || !payload->image_size) return 6;

    u8* image = payload->image_base;
    u32 ntOffset = *(u32*)(image + 0x3C);
    u8* nt = image + ntOffset;
    if (*(u32*)nt != 0x00004550) return 7;

    const u64 preferredBase = *(u64*)(nt + 0x18 + 0x18);
    const u32 sizeOfImage = *(u32*)(nt + 0x18 + 0x38);
    const u32 sectionCount = *(u16*)(nt + 0x6);
    const u32 optionalHeaderSize = *(u16*)(nt + 0x14);
    StubSectionHeader* sections = (StubSectionHeader*)(nt + 0x18 + optionalHeaderSize);

    // 3. Relocations — zero for our own image by policy (ADR-1), but a client
    //    build change could reintroduce them, so they are applied rather than
    //    assumed away.
    stub_apply_relocations(image, (i64)(image - preferredBase));

    // 4. Imports.
    const u32 failures = stub_resolve_imports(image, ldr_load, ldr_get);
    if (failures) return 8;

    // 5. Section protections from the headers.
    for (u32 i = 0; i < sectionCount; ++i) {
        StubSectionHeader* section = &sections[i];
        if (!section->virtual_size) continue;
        const u32 characteristics = *(u32*)((u8*)section + 0x24);
        u32 protection = 0x02;                                  // PAGE_READONLY
        if (characteristics & 0x80000000) protection = 0x80;    // writable, no execute
        if (characteristics & 0x20000000) protection = 0x20;    // execute, no write
        if ((characteristics & 0x20000000) && (characteristics & 0x80000000)) protection = 0x40;
        u32 previous = 0;
        stub_virtual_protect(image + section->virtual_address, section->virtual_size,
                             protection, &previous);
    }

    // 6. Find the exported entry and call it.
    void* entry = 0;
    if (payload->entry_rva) {
        entry = image + payload->entry_rva;
    } else {
        entry = stub_get_proc(image, PHETAMINE_STUB_EXPORT);
    }
    if (!entry) return 9;

    typedef void (__stdcall *EntryFn)(void*, void*, void*);
    EntryFn function = (EntryFn)entry;
    function(payload->param_block, payload, image);
    (void)sizeOfImage;
    return 0;
}
