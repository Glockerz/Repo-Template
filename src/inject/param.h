// inject/param.h — the block the injector hands to the shellcode stub.
//
// Layout rules:
//   * POD only, fixed-size fields, no pointers (the block is written into the
//     target with WriteProcessMemory and must mean the same thing at any base);
//   * magic + size are checked before anything is trusted;
//   * every string is a NUL-terminated UTF-16 path, truncated rather than
//     rejected, because a long workspace path must never fail an injection.
#pragma once

#include <cstdint>

namespace phetamine::inject {

inline constexpr uint32_t kParamMagic   = 0x504D5450;  // 'PTMP' — phetamine param
inline constexpr uint32_t kParamVersion = 1;

inline constexpr uint32_t kFlagManualMap  = 1u << 0;   // the stub mapped us, not the loader
inline constexpr uint32_t kFlagKeepLoaded = 1u << 1;   // reserved: do not unload on failure

struct ParamBlock {
    uint32_t magic = kParamMagic;
    uint32_t version = kParamVersion;
    uint32_t structSize = sizeof(ParamBlock);
    uint32_t flags = 0;

    uint64_t reserved0 = 0;

    wchar_t  workspace[260] = {};   // MAX_PATH, NUL-terminated
    wchar_t  pipeName[128] = {};    // reserved: the UI may pin the name
    uint32_t pid = 0;               // the target process this block was written to
    uint32_t reserved1 = 0;
    uint64_t reserved2 = 0;
};

static_assert(sizeof(ParamBlock) <= 1024, "the param block must stay tiny — it is written per inject");
static_assert(sizeof(ParamBlock) % 8 == 0, "keep the block 8-byte aligned for the stub");

// Validates a block that was written by another process. Returns false when the
// magic/version/size disagree — in that case the caller falls back to defaults
// instead of trusting garbage.
inline bool Validate(const ParamBlock* block) {
    if (!block) return false;
    if (block->magic != kParamMagic) return false;
    if (block->version != kParamVersion) return false;
    if (block->structSize < sizeof(ParamBlock)) return false;
    return true;
}

}  // namespace phetamine::inject
