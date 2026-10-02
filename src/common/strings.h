// common/strings.h — reading the client's strings safely.
//
// Roblox's `Instance.Name` and `ClassDescriptor.ClassName` are std::string-like
// objects with small-string optimisation. The layout is derived by
// cross-checking two independent public sources for two different builds
// (documented in docs/OFFSETS.md §3):
//
//   SSO object  { union { char buf[16]; char* ptr; }; size_t length; size_t capacity; }
//   length offset within the object = Misc.StringLength (0x10 in the 02c37 dump)
//
// and the *location* of that object moves between builds:
//   build d599f7fc: instance + 0xB0 directly
//   build 02c37bc5: instance + NameContainer(0x70) + Name(0x08)
//
// So nothing here hardcodes a path: the walker passes the resolved SSO base in.
#pragma once

#include <windows.h>
#include <cstdint>
#include <string>
#include <string_view>
#include "common/seh.h"

namespace phetamine::str {

// Maximum characters we will ever believe from an in-process string. Anything
// larger is a mis-resolved pointer, not a name.
inline constexpr size_t kMaxLen = 512;

// Reads an SSO string whose object starts at `ssoBase`, using the resolved
// length field offset. Empty string on any failed read or implausible length.
inline std::string ReadSso(uintptr_t ssoBase, uintptr_t lengthOffset) {
    if (ssoBase < 0x10000) return {};
    uintptr_t len = 0;
    if (!seh::TryRead<uintptr_t>(ssoBase + lengthOffset, len)) return {};
    if (len == 0) return {};
    if (len > kMaxLen) return {};

    // <= 15 (the inline buffer) is a direct read; longer is heap-backed.
    if (len <= 15) {
        char buf[16]{};
        if (!seh::TryReadBytes(ssoBase, buf, len)) return {};
        return std::string(buf, len);
    }
    uintptr_t ptr = 0;
    if (!seh::TryReadPtr(ssoBase, ptr)) return {};
    if (ptr < 0x10000) return {};
    if (!seh::IsReadable(ptr, len + 1)) return {};
    std::string out;
    out.resize(len);
    if (!seh::TryReadBytes(ptr, out.data(), len)) return {};
    return out;
}

// UTF-16 SSO variant (some engine strings are wide).
inline std::wstring ReadSsoW(uintptr_t ssoBase, uintptr_t lengthOffset) {
    if (ssoBase < 0x10000) return {};
    uintptr_t len = 0;
    if (!seh::TryRead<uintptr_t>(ssoBase + lengthOffset, len)) return {};
    if (len == 0 || len > kMaxLen) return {};
    std::wstring out;
    out.resize(len);
    if (len <= 7) {
        if (!seh::TryReadBytes(ssoBase, out.data(), len * sizeof(wchar_t))) return {};
    } else {
        uintptr_t ptr = 0;
        if (!seh::TryReadPtr(ssoBase, ptr)) return {};
        if (ptr < 0x10000) return {};
        if (!seh::TryReadBytes(ptr, out.data(), len * sizeof(wchar_t))) return {};
    }
    return out;
}

// A NUL-terminated C string at a known address, length-capped.
inline std::string ReadCString(uintptr_t addr, size_t maxLen = kMaxLen) {
    if (addr < 0x10000) return {};
    char buf[256];
    std::string out;
    for (size_t off = 0; off < maxLen; off += sizeof(buf)) {
        size_t chunk = (maxLen - off) < sizeof(buf) ? (maxLen - off) : sizeof(buf);
        if (!seh::TryReadBytes(addr + off, buf, chunk)) break;
        for (size_t i = 0; i < chunk; i++) {
            if (buf[i] == '\0') return out;
            out.push_back(buf[i]);
        }
    }
    return out;
}

// UTF-8 ⇄ UTF-16. Inline rather than a separate translation unit: both are three
// lines around one API call, and every translation unit that reads engine strings
// already includes this header.
inline std::wstring Widen(std::string_view s) {
    if (s.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), size);
    return out;
}

inline std::string Narrow(std::wstring_view s) {
    if (s.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                        nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), size,
                        nullptr, nullptr);
    return out;
}

inline bool IEquals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

inline bool StartsWith(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

}  // namespace phetamine::str
