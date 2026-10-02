// common/seh.h — SEH-guarded memory access.
//
// Everything in this DLL that dereferences a pointer into the client's address
// space goes through here. A bad offset in-process is not a failed call, it is
// a client crash, so the rule is: probe, then probe again, then read.
//
// Build note: this translation unit family requires /EHa (see CMakeLists.txt).
// MSVC refuses __try inside a function that requires object unwinding under
// /EHsc (C2712); /EHa is the documented workaround, and it also lets our code
// catch SEH faults as C++ exceptions where that is useful. The helpers below
// keep POD-only signatures so they never need unwinding themselves.
#pragma once

#include <windows.h>
#include <cstdint>
#include <cstring>

namespace phetamine::seh {

// ---- own-image bookkeeping (set by the entry point / mapper) -----------------
void  SetOwnImageRange(uintptr_t base, size_t size);
bool  IsOwnCode(uintptr_t addr);

// ---- guarded primitives ------------------------------------------------------
template <typename T>
inline bool TryRead(uintptr_t addr, T& out) {
    if (addr < 0x10000) return false;
    __try {
        out = *reinterpret_cast<const T*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

inline bool TryReadPtr(uintptr_t addr, uintptr_t& out) {
    return TryRead<uintptr_t>(addr, out);
}

template <typename T>
inline bool TryWrite(uintptr_t addr, const T& value) {
    if (addr < 0x10000) return false;
    __try {
        *reinterpret_cast<T*>(addr) = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

inline bool TryReadBytes(uintptr_t addr, void* dst, size_t n) {
    if (addr < 0x10000 || !dst || n == 0) return false;
    __try {
        std::memcpy(dst, reinterpret_cast<const void*>(addr), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

inline bool TryWriteBytes(uintptr_t addr, const void* src, size_t n) {
    if (addr < 0x10000 || !src || n == 0) return false;
    __try {
        std::memcpy(reinterpret_cast<void*>(addr), src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// POD-only guarded call for resolved function pointers (VM entry points, engine
// helpers). Never pass anything with a destructor through here.
template <typename Ret, typename... Args>
inline bool GuardedCall(Ret (*fn)(Args...), Args... args) {
    if (!fn) return false;
    __try {
        fn(args...);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---- page queries ------------------------------------------------------------
bool IsReadable(uintptr_t addr, size_t n);
bool IsExecutable(uintptr_t addr, size_t n);
bool IsWritable(uintptr_t addr, size_t n);

// ---- VEH backstop ------------------------------------------------------------
// If an access violation ever happens inside our own code, the VEH sees it
// first so it can be counted and reported (and, past a threshold, trigger a
// clean unload) instead of turning into a process death the user cannot
// attribute. Faults we cannot explain are always passed on (CONTINUE_SEARCH):
// swallowing someone else's exception would be worse than crashing.
bool     InstallBackstop();
uint32_t FaultsFromOwnCode();
bool     ShouldUnloadOnFaults();

}  // namespace phetamine::seh
