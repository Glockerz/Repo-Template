// memory/scanner.h — IDA-pattern scanning plus the layered resolver.
//
// Resolution order for anything the offset table cannot answer (docs/OFFSETS.md):
//   1. an address cache hit for this build  (fast, but still probed by the caller)
//   2. a signature from memory/sigs.h       (rejected if absent or ambiguous)
//   3. nothing                              ⇒ caller disables the capability
#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "memory/pe.h"

namespace phetamine::scan {

struct Pattern {
    std::vector<uint8_t> bytes;
    std::vector<bool>    mask;   // true = must match, false = wildcard
    bool valid = false;
};

// "48 8B ?? ?? 89" → bytes + mask. `??` and `?` are wildcards.
Pattern Parse(const char* ida);

struct Match {
    uintptr_t address = 0;   // absolute
    uintptr_t rva = 0;
    bool ambiguous = false;  // more matches than the caller allowed
    int   count = 0;
};

// Scans every executable section. `maxMatches` = 0 means "1"; anything beyond
// it sets `ambiguous` and returns the first match with a flag, so the caller can
// refuse it (docs/OFFSETS.md: an ambiguous pattern is not an answer).
Match FindInExecutable(const pe::ImageInfo& img, const Pattern& p, int maxMatches = 1);

// Resolves a Sig entry (pattern + optional RIP-relative fixup). Empty patterns
// and ambiguous matches both return a zero Match.
Match ResolveSig(const pe::ImageInfo& img, const char* key);

// Reads the 32-bit displacement at `match + dispOffset` and returns
// `match + instrLen + disp`. Used for `mov rax, [rip+disp]`-style globals.
uintptr_t FollowRipRelative(uintptr_t match, int dispOffset, int instrLen);

}  // namespace phetamine::scan
