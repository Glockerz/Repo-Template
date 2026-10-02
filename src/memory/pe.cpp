#include "memory/pe.h"
#include "common/seh.h"
#include "common/log.h"

#include <cstring>
#include <vector>

namespace phetamine::pe {
namespace {

template <typename T>
bool ReadAt(uintptr_t addr, T& out) { return seh::TryRead<T>(addr, out); }

}  // namespace

ImageInfo Inspect(uintptr_t moduleBase) {
    ImageInfo img{};
    img.base = moduleBase;
    if (moduleBase < 0x10000) return img;

    uint16_t mz = 0;
    if (!ReadAt<uint16_t>(moduleBase, mz) || mz != 0x5A4D) return img;

    int32_t e_lfanew = 0;
    if (!ReadAt<int32_t>(moduleBase + 0x3C, e_lfanew) || e_lfanew <= 0 || e_lfanew > 0x1000) return img;

    const uintptr_t nt = moduleBase + static_cast<uintptr_t>(e_lfanew);
    uint32_t sig = 0;
    if (!ReadAt<uint32_t>(nt, sig) || sig != 0x00004550) return img;

    uint16_t machine = 0, sectionCount = 0, optSize = 0;
    if (!ReadAt<uint16_t>(nt + 4, machine) || !ReadAt<uint16_t>(nt + 6, sectionCount) ||
        !ReadAt<uint16_t>(nt + 20, optSize)) {
        return img;
    }
    if (machine != IMAGE_FILE_MACHINE_AMD64 || sectionCount == 0 || sectionCount > 96) return img;

    const uintptr_t opt = nt + 24;
    uint16_t magic = 0;
    if (!ReadAt<uint16_t>(opt, magic) || magic != 0x20B) return img;

    uint32_t sizeOfImage = 0, entryPoint = 0;
    if (!ReadAt<uint32_t>(opt + 0x38, sizeOfImage) || !ReadAt<uint32_t>(opt + 0x10, entryPoint)) return img;
    if (sizeOfImage < 0x1000 || sizeOfImage > (768u << 20)) return img;

    img.sizeOfImage = sizeOfImage;
    img.entryPoint = entryPoint;
    img.sectionCount = sectionCount;

    const uintptr_t secBase = opt + optSize;
    for (int i = 0; i < sectionCount; i++) {
        const uintptr_t s = secBase + static_cast<uintptr_t>(i) * 40;
        char name[9]{};
        if (!seh::TryReadBytes(s, name, 8)) return img;
        Section& out = img.sections[i];
        std::memcpy(out.name, name, 8);
        out.name[8] = '\0';
        if (!ReadAt<uint32_t>(s + 8, out.vSize) || !ReadAt<uint32_t>(s + 12, out.rva) ||
            !ReadAt<uint32_t>(s + 16, out.rawSize) || !ReadAt<uint32_t>(s + 36, out.characteristics)) {
            return img;
        }
    }
    img.valid = true;
    return img;
}

const Section* FindSection(const ImageInfo& img, const char* name) {
    for (int i = 0; i < img.sectionCount; i++) {
        if (std::strcmp(img.sections[i].name, name) == 0) return &img.sections[i];
    }
    return nullptr;
}

const Section* SectionOf(const ImageInfo& img, uintptr_t rva) {
    for (int i = 0; i < img.sectionCount; i++) {
        const Section& s = img.sections[i];
        if (rva >= s.rva && rva < s.end()) return &s;
    }
    return nullptr;
}

std::string FindVersionString(const ImageInfo& img) {
    if (!img.valid) return {};
    constexpr char kNeedle[] = "version-";
    constexpr size_t kNeedleLen = sizeof(kNeedle) - 1;
    constexpr size_t kChunk = 1u << 20;

    std::vector<char> buf(kChunk + 64);
    for (int i = 0; i < img.sectionCount; i++) {
        const Section& s = img.sections[i];
        if (!s.readable() || s.executable()) continue;            // data sections only
        const size_t len = s.vSize ? s.vSize : s.rawSize;
        if (len == 0 || len > (256u << 20)) continue;
        for (size_t off = 0; off < len; off += kChunk) {
            size_t want = len - off;
            if (want > kChunk) want = kChunk;
            if (!seh::TryReadBytes(img.base + s.rva + off, buf.data(), want)) break;
            buf[want] = '\0';
            for (size_t j = 0; j + kNeedleLen + 16 <= want; j++) {
                if (std::memcmp(buf.data() + j, kNeedle, kNeedleLen) != 0) continue;
                const char* hex = buf.data() + j + kNeedleLen;
                bool ok = true;
                for (int h = 0; h < 16; h++) {
                    const char c = hex[h];
                    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) { ok = false; break; }
                }
                if (ok) return std::string(kNeedle, kNeedleLen) + std::string(hex, 16);
            }
        }
    }
    return {};
}

}  // namespace phetamine::pe
