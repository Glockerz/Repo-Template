#include "common/log.h"
#include "common/seh.h"

#include <atomic>

namespace phetamine::log {
namespace {

std::atomic<Sink> g_sink{ nullptr };

char LevelTag(Level level) {
    switch (level) {
        case Level::Warn:  return 'W';
        case Level::Error: return 'E';
        case Level::Fatal: return 'F';
        default:           return 'I';
    }
}

// One shared formatting buffer per call: this is a stack buffer, so the sink
// contract ("valid only for the duration of the call") is exact.
constexpr size_t kLineCapacity = 900;   // matches the IPC log ring's line size

}  // namespace

void SetSink(Sink sink) { g_sink.store(sink, std::memory_order_release); }
bool HasSink() { return g_sink.load(std::memory_order_acquire) != nullptr; }

void VWrite(Level level, const char* fmt, va_list args) {
    char buffer[kLineCapacity];
    int written = 0;
    if (fmt && *fmt) {
        written = std::vsnprintf(buffer + 2, kLineCapacity - 3, fmt, args);
        if (written < 0) written = 0;
        const int cap = static_cast<int>(kLineCapacity) - 3;
        if (written > cap) written = cap;
    }
    buffer[0] = LevelTag(level);
    buffer[1] = ' ';
    buffer[written + 2] = '\0';

    if (Sink sink = g_sink.load(std::memory_order_acquire)) {
        sink(level, buffer);
        return;
    }
    // Before a sink exists (very early init) and in debug builds, the debugger
    // output is the only place left. It is deliberately not a file: a log file is
    // the UI's job through the IPC ring.
    ::OutputDebugStringA(buffer);
    ::OutputDebugStringA("\n");
}

void Write(Level level, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    VWrite(level, fmt, args);
    va_end(args);
}

void StageError(const char* stage, const char* fmt, ...) {
    // The UI greps for the literal "ERROR:" prefix and renders the stage name it
    // finds after the colon; that string is part of the IPC contract.
    char message[kLineCapacity];
    va_list args;
    va_start(args, fmt);
    int written = std::vsnprintf(message, sizeof(message) - 1, fmt, args);
    va_end(args);
    if (written < 0) written = 0;
    message[written] = '\0';

    if (stage && *stage) Write(Level::Fatal, "ERROR:%s %s", stage, message);
    else Write(Level::Fatal, "ERROR:Unknown %s", message);
}

void DebugOutputSink(Level level, const char* line) {
    (void)level;
    if (!line) return;
    ::OutputDebugStringA(line);
    ::OutputDebugStringA("\n");
}

}  // namespace phetamine::log
