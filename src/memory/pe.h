// memory/pe.h — minimal PE introspection for the *client* module.
//
// Only what the resolver needs: image size (to bound scans), section table (to
// scan executable sections), and the version-resource check.
#pragma once

#include <windows.h>
#include <cstdint>
#include <string>
#include "common/seh.h"

namespace phetamine::pe {

struct Section {
    char     name[9]{};
    uint32_t rva = 0;
    uint32_t vSize = 0;
    uint32_t rawSize = 0;
    uint32_t characteristics = 0;

    bool executable() const { return (characteristics & IMAGE_SCN_MEM_EXECUTE) != 0; }
    bool readable()   const { return (characteristics & IMAGE_SCN_MEM_READ) != 0; }
    uint32_t end()    const { return rva + (vSize ? vSize : rawSize); }
};

struct ImageInfo {
    uintptr_t base = 0;
    size_t    sizeOfImage = 0;
    uint32_t  entryPoint = 0;
    int       sectionCount = 0;
    Section   sections[96]{};
    bool      valid = false;
};

// Reads headers through SEH; returns {valid=false} on anything implausible.
ImageInfo Inspect(uintptr_t moduleBase);
const Section* FindSection(const ImageInfo& img, const char* name);
const Section* SectionOf(const ImageInfo& img, uintptr_t rva);

// `version-<16 hex>` — the client's public build id. Roblox keeps it as a
// literal in the image; scanning for it is how we learn which build we are in
// without asking the user. Returns "" when not found.
std::string FindVersionString(const ImageInfo& img);

}  // namespace phetamine::pe
