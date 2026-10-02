// memory/offsets.h — the offset table API.
//
// Resolution law (docs/OFFSETS.md): version table → AOB scan → probed offset →
// DISABLE. There is no "plausible default": `Get()` returns 0 for anything the
// table does not carry, and every consumer must treat 0 as "feature off".
//
// Kind semantics:
//   Kind::rva    → Get() returns moduleBase + value  (a global inside the image)
//   Kind::offset → Get() returns value unchanged     (a field offset from an object)
#pragma once

#include <cstdint>
#include <cstddef>

namespace off {

enum class Kind : uint8_t { rva, offset };

struct Row {
    const char* key;
    uint64_t    value;
    Kind        kind;
    const char* from;   // provenance: the dumper path this came from
};

enum class Status : uint8_t { Absent, Found, Probed, Failed };

struct Stats {
    size_t total = 0;
    size_t resolved = 0;
    size_t probed = 0;
    size_t failed = 0;
};

// Loads the compiled-in table. `moduleBase` is the client image base used for
// Kind::rva rows. Safe to call once per bind (cheap, idempotent).
void Load(uintptr_t moduleBase);

// 0 ⇒ absent or unresolvable. Callers must fail closed on 0.
uintptr_t Get(const char* key);

Status StatusOf(const char* key);
bool   Probed(const char* key);
void   MarkProbed(const char* key, bool ok);   // false ⇒ entry becomes Failed

Stats  GetStats();
void   Dump();                                  // logs every row + probe state

// ---- build identification ----------------------------------------------------
const wchar_t* ExpectedVersion();   // compiled in from offsets_table.inc
const char*    RunningVersion();    // scanned out of the live client image
bool           VersionMatches();    // expected == running ("" running ⇒ false)

// ---- resolution cache --------------------------------------------------------
// Addresses that were found by AOB scan are persisted per build so the next
// boot is fast. The cache is advisory only: a cached value is still probed
// before use, and a failed probe evicts it (docs/OFFSETS.md §6.3).
void LoadAddressCache(const wchar_t* path);
bool SaveAddressCache(const wchar_t* path);
bool CacheGet(const char* key, uintptr_t& out);
void CachePut(const char* key, uintptr_t value);

}  // namespace off
