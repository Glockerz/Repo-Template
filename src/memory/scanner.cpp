#include "memory/scanner.h"
#include "memory/pe.h"
#include "memory/sigs.h"
#include "memory/offsets.h"
#include "common/log.h"
#include "common/seh.h"

#include <cstring>
#include <vector>

namespace phetamine::scan {
namespace {

constexpr size_t kChunk = 1u << 20;

int HexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool MatchAt(const uint8_t* hay, const Pattern& p) {
    for (size_t i = 0; i < p.bytes.size(); i++) {
        if (p.mask[i] && hay[i] != p.bytes[i]) return false;
    }
    return true;
}

}  // namespace

Pattern Parse(const char* ida) {
    Pattern p;
    if (!ida || !*ida) return p;
    const char* c = ida;
    while (*c) {
        while (*c == ' ' || *c == '\t') c++;
        if (!*c) break;
        if (*c == '?' || *c == 'x') {          // wildcard token
            p.bytes.push_back(0);
            p.mask.push_back(false);
            while (*c && *c != ' ' && *c != '\t') c++;   // consume "??" / "?"
            continue;
        }
        const int hi = HexNibble(c[0]);
        const int lo = c[1] ? HexNibble(c[1]) : -1;
        if (hi < 0 || lo < 0) {
            log::Warn("scanner: bad pattern token at '%s'", c);
            return Pattern{};
        }
        p.bytes.push_back(static_cast<uint8_t>((hi << 4) | lo));
        p.mask.push_back(true);
        c += 2;
    }
    p.valid = !p.bytes.empty();
    return p;
}

Match FindInExecutable(const pe::ImageInfo& img, const Pattern& p, int maxMatches) {
    Match result{};
    if (!img.valid || !p.valid) return result;
    if (maxMatches <= 0) maxMatches = 1;

    std::vector<uint8_t> buf(kChunk + p.bytes.size() + 16);
    for (int s = 0; s < img.sectionCount; s++) {
        const pe::Section& sec = img.sections[s];
        if (!sec.executable() || !sec.readable()) continue;
        size_t len = sec.vSize ? sec.vSize : sec.rawSize;
        if (len < p.bytes.size()) continue;
        const uintptr_t base = img.base + sec.rva;

        for (size_t off = 0; off + p.bytes.size() <= len; off += kChunk) {
            size_t want = len - off;
            if (want > kChunk + p.bytes.size()) want = kChunk + p.bytes.size();
            if (!seh::TryReadBytes(base + off, buf.data(), want)) break;
            for (size_t i = 0; i + p.bytes.size() <= want; i++) {
                if (!MatchAt(buf.data() + i, p)) continue;
                result.count++;
                if (result.address == 0) {
                    result.address = base + off + i;
                    result.rva = sec.rva + static_cast<uint32_t>(off + i);
                }
                if (result.count > maxMatches) {
                    result.ambiguous = true;
                    return result;
                }
            }
        }
    }
    return result;
}

uintptr_t FollowRipRelative(uintptr_t match, int dispOffset, int instrLen) {
    int32_t disp = 0;
    if (!seh::TryRead<int32_t>(match + static_cast<uintptr_t>(dispOffset), disp)) return 0;
    return match + static_cast<uintptr_t>(instrLen) + static_cast<uintptr_t>(static_cast<intptr_t>(disp));
}

Match ResolveSig(const pe::ImageInfo& img, const char* key) {
    Match none{};
    for (const sigs::Sig& sig : sigs::kSigs) {
        if (std::strcmp(sig.key, key) != 0) continue;

        if (!sig.bytes || !*sig.bytes) {
            // Not derived for this build — this is the documented, expected state
            // for a fresh tree. Say so once, at debug level, and move on.
            log::Info("scan: '%s' has no pattern for build %s (gates: %s) — feature stays off",
                      key, off::RunningVersion(), sig.gates);
            return none;
        }

        const Pattern p = Parse(sig.bytes);
        if (!p.valid) {
            log::Error("scan: '%s' pattern did not parse ('%s')", key, sig.bytes);
            return none;
        }
        Match m = FindInExecutable(img, p, sig.maxMatches <= 0 ? 1 : sig.maxMatches);
        if (m.address == 0) {
            log::Warn("scan: '%s' pattern matched nothing — client build differs from the one "
                      "this signature was derived on", key);
            return none;
        }
        if (m.ambiguous) {
            log::Warn("scan: '%s' pattern matched %d+ sites — refusing an ambiguous answer", key, m.count);
            return none;
        }
        if (sig.ripDisp > 0) {
            const uintptr_t target = FollowRipRelative(m.address, sig.ripDisp, sig.ripLen);
            if (!target) {
                log::Warn("scan: '%s' RIP-relative fixup failed", key);
                return none;
            }
            m.address = target;
            m.rva = static_cast<uint32_t>(target - img.base);
        }
        log::Info("scan: '%s' → %p (rva 0x%X)", key, reinterpret_cast<void*>(m.address), m.rva);
        return m;
    }
    log::Warn("scan: '%s' is not in the signature table", key);
    return none;
}

}  // namespace phetamine::scan
