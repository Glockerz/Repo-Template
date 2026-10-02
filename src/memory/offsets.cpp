#include "memory/offsets.h"
#include "memory/pe.h"
#include "common/log.h"
#include "common/seh.h"

#include <windows.h>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>

// The generated table. Offsets.h defines Row/Kind before this include point.
// (CMake fails the build if the file is missing — see src/CMakeLists.txt.)
#include "memory/offsets_table.inc"

namespace off {
namespace {

uintptr_t g_moduleBase = 0;
pe::ImageInfo g_image{};

struct CacheEntry {
    const char* key;
    uintptr_t   value;
};

struct Slot {
    const Row*    row;
    uintptr_t     value;    // absolute for rva rows, raw for offset rows
    Status        status;
    bool          probedOk;
};

std::vector<Slot> g_slots;
std::vector<CacheEntry> g_cache;   // AOB results, keyed by internal key
std::string g_runningVersion;

Slot* Find(const char* key) {
    for (auto& s : g_slots) {
        if (std::strcmp(s.row->key, key) == 0) return &s;
    }
    return nullptr;
}

// Chunked search of an image range for `version-<16 hex>`. Reading a committed
// image through SEH is safe; we never touch unreadable ranges because the scan
// is bounded by SizeOfImage, and a bad read just ends the search.
std::string ScanVersionString(const pe::ImageInfo& img) {
    constexpr size_t kChunk = 1u << 20;
    constexpr char kNeedle[] = "version-";
    constexpr size_t kNeedleLen = sizeof(kNeedle) - 1;
    const size_t total = img.sizeOfImage;
    if (total < kNeedleLen + 16 || total > (512u << 20)) return {};

    std::vector<char> buf(kChunk + kNeedleLen + 32);
    for (size_t off = 0; off + kNeedleLen + 16 < total; off += kChunk) {
        size_t want = kChunk;
        if (off + want > total) want = total - off;
        if (!seh::TryReadBytes(img.base + off, buf.data(), want)) continue;
        buf[want] = '\0';
        for (size_t i = 0; i + kNeedleLen + 16 <= want; i++) {
            if (std::memcmp(buf.data() + i, kNeedle, kNeedleLen) != 0) continue;
            const char* hex = buf.data() + i + kNeedleLen;
            bool ok = true;
            for (int h = 0; h < 16; h++) {
                const char c = hex[h];
                if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) { ok = false; break; }
            }
            if (ok) return std::string(kNeedle, kNeedleLen) + std::string(hex, 16);
        }
    }
    return {};
}

bool IsHex16(const char* s) {
    if (!s) return false;
    for (int i = 0; i < 16; i++) {
        const char c = s[i];
        if (!c) return false;
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return s[16] == '\0';
}

}  // namespace

void Load(uintptr_t moduleBase) {
    g_moduleBase = moduleBase;
    g_image = pe::Inspect(moduleBase);
    if (!g_image.valid) {
        log::Error("offsets: client image headers did not parse (base=%p)", (void*)moduleBase);
    }
    g_runningVersion = g_image.valid ? pe::FindVersionString(g_image) : std::string{};
    if (g_runningVersion.empty()) {
        log::Warn("offsets: could not find a version-<16hex> literal in the client image; "
                  "table values will be used but the build cannot be confirmed");
    }

    g_slots.clear();
    g_slots.reserve(table::kCount);
    for (const Row& row : table::kRows) {
        Slot slot{ &row, 0, Status::Absent, false };
        if (row.value != 0) {
            slot.value = (row.kind == Kind::rva) ? (g_moduleBase + static_cast<uintptr_t>(row.value))
                                                 : static_cast<uintptr_t>(row.value);
            slot.status = Status::Found;
        }
        g_slots.push_back(slot);
    }

    const Stats s = GetStats();
    const bool matches = VersionMatches();
    log::Info("offsets: %zu rows (%zu resolved) · table=%ls · running=%s · match=%s",
              s.total, s.resolved, ExpectedVersion(),
              g_runningVersion.empty() ? "(unknown)" : g_runningVersion.c_str(),
              matches ? "yes" : "no");
    if (!matches && !g_runningVersion.empty()) {
        log::Warn("offsets: table is for %ls but the client is %s — AOB/cache resolution "
                  "is the only trustworthy source this session", ExpectedVersion(),
                  g_runningVersion.c_str());
    }
}

uintptr_t Get(const char* key) {
    Slot* slot = Find(key);
    if (!slot || slot->status == Status::Absent) return 0;
    if (slot->status == Status::Failed) return 0;   // failed probe ⇒ feature off
    return slot->value;
}

Status StatusOf(const char* key) {
    Slot* slot = Find(key);
    return slot ? slot->status : Status::Absent;
}

bool Probed(const char* key) {
    Slot* slot = Find(key);
    return slot && slot->status == Status::Probed && slot->probedOk;
}

void MarkProbed(const char* key, bool ok) {
    Slot* slot = Find(key);
    if (!slot) {
        log::Warn("offsets: probe reported for unknown key '%s'", key);
        return;
    }
    slot->probedOk = ok;
    slot->status = ok ? Status::Probed : Status::Failed;
    if (!ok) log::Error("offsets: probe FAILED for %s (0x%llX) — dependent features disabled",
                        key, static_cast<unsigned long long>(slot->value));
}

Stats GetStats() {
    Stats s{};
    s.total = g_slots.size();
    for (const auto& slot : g_slots) {
        if (slot.status == Status::Found || slot.status == Status::Probed) s.resolved++;
        if (slot.status == Status::Probed) s.probed++;
        if (slot.status == Status::Failed) s.failed++;
    }
    return s;
}

void Dump() {
    log::Info("---- offset dump (client %s) ----",
              g_runningVersion.empty() ? "unknown" : g_runningVersion.c_str());
    for (const auto& slot : g_slots) {
        const char* state =
            slot.status == Status::Probed ? "probed" :
            slot.status == Status::Found  ? "found"  :
            slot.status == Status::Failed ? "FAILED" : "absent";
        log::Info("  %-44s 0x%08llX  %-6s %-7s <- %s",
                  slot.row->key, static_cast<unsigned long long>(slot.value),
                  slot.row->kind == Kind::rva ? "rva" : "offset", state, slot.row->from);
    }
    const Stats s = GetStats();
    log::Info("---- %zu rows · %zu resolved · %zu probed · %zu failed ----",
              s.total, s.resolved, s.probed, s.failed);
}

const wchar_t* ExpectedVersion() { return PHETAMINE_ROBLOX_VERSION; }

const char* RunningVersion() { return g_runningVersion.c_str(); }

bool VersionMatches() { return !g_runningVersion.empty() && IsHex16(g_runningVersion.c_str() + 8); }

// ---- cache -------------------------------------------------------------------

void LoadAddressCache(const wchar_t* path) {
    if (!path || !*path) return;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > (1 << 20)) {
        CloseHandle(file);
        return;
    }
    std::string text(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    const BOOL ok = ReadFile(file, text.data(), static_cast<DWORD>(text.size()), &read, nullptr);
    CloseHandle(file);
    if (!ok) return;

    // format: one `key=0xHEX` per line, `#` comments. Deliberately boring so a
    // human can hand-edit it while chasing a patch.
    size_t pos = 0;
    int loaded = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        if (line.empty() || line[0] == '#') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        const uintptr_t value = static_cast<uintptr_t>(std::strtoull(line.c_str() + eq + 1, nullptr, 0));
        if (value) { CachePut(key.c_str(), value); loaded++; }
    }
    if (loaded) log::Info("offsets: address cache loaded (%d entries)", loaded);
}

bool SaveAddressCache(const wchar_t* path) {
    if (!path || !*path || g_cache.empty()) return false;
    std::string out = "# PHETAMINE address cache — probed on use; a failed probe evicts.\n";
    out += "# build: " + (g_runningVersion.empty() ? std::string("unknown") : g_runningVersion) + "\n";
    for (const auto& e : g_cache) {
        char line[256];
        std::snprintf(line, sizeof(line), "%s=0x%llX\n", e.key, static_cast<unsigned long long>(e.value));
        out += line;
    }
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = WriteFile(file, out.data(), static_cast<DWORD>(out.size()), &written, nullptr);
    CloseHandle(file);
    return ok && written == out.size();
}

bool CacheGet(const char* key, uintptr_t& out) {
    for (const auto& e : g_cache) {
        if (std::strcmp(e.key, key) == 0) { out = e.value; return true; }
    }
    return false;
}

void CachePut(const char* key, uintptr_t value) {
    for (auto& e : g_cache) {
        if (std::strcmp(e.key, key) == 0) { e.value = value; return; }
    }
    // keys are program literals (never freed); the vector owns nothing
    g_cache.push_back(CacheEntry{ key, value });
}

}  // namespace off
