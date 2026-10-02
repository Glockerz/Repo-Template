// common/log.h — tiny logging façade.
//
// The DLL must be able to log before IPC exists (and if IPC never comes up), so
// the sink is a function pointer the IPC layer installs later. `log::Write` is
// allocation-free in the common path: it formats into a stack buffer and hands
// the sink a pointer that is valid only for the duration of the call.
//
// The scheduler drain path calls only log::Probe (SEH-safe, no sink traffic) so
// a chatty sink can never stall a frame.
#pragma once

#include <windows.h>
#include <cstdarg>
#include <cstdio>

namespace phetamine::log {

enum class Level : uint8_t { Info, Warn, Error, Fatal };

// Sink contract: must be thread-safe, must not throw, must copy what it needs.
// Valid only for the duration of the call.
using Sink = void (*)(Level level, const char* line);

void SetSink(Sink sink);
bool HasSink();

void Write(Level level, const char* fmt, ...);
void VWrite(Level level, const char* fmt, va_list args);

inline void Info(const char* fmt, ...) {
    va_list a; va_start(a, fmt); VWrite(Level::Info, fmt, a); va_end(a);
}
inline void Warn(const char* fmt, ...) {
    va_list a; va_start(a, fmt); VWrite(Level::Warn, fmt, a); va_end(a);
}
inline void Error(const char* fmt, ...) {
    va_list a; va_start(a, fmt); VWrite(Level::Error, fmt, a); va_end(a);
}
inline void Fatal(const char* fmt, ...) {
    va_list a; va_start(a, fmt); VWrite(Level::Fatal, fmt, a); va_end(a);
}

// Stage-tagged error: emitted as the literal `ERROR:<stage>` prefix the UI
// parses, so the user sees which stage broke instead of a silent hang.
void StageError(const char* stage, const char* fmt, ...);

// Last-resort sink used before IPC exists: OutputDebugStringA + optional
// console (PHETAMINE_DEBUG builds only, gated by the caller).
void DebugOutputSink(Level level, const char* line);

}  // namespace phetamine::log
