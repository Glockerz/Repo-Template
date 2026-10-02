#include "ipc/protocol.h"
#include "common/log.h"

#include <windows.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace phetamine::ipc {
namespace {

struct Slot {
    std::atomic<uint32_t> published{ 0 };   // 0 = empty, else seq+1
    Level level = Level::Info;
    char  text[kLogLineMax]{};
};

Slot g_slots[kLogRingSlots];
std::atomic<uint32_t> g_head{ 0 };      // consumer
std::atomic<uint32_t> g_tail{ 0 };      // producer
std::atomic<uint32_t> g_dropped{ 0 };

}  // namespace

const char* OpName(Op op) {
    switch (op) {
#define X(value, name) case Op::name: return #name;
        PHETAMINE_OPCODES(X)
#undef X
        default: return "Unknown";
    }
}

void LogLine(Level level, const char* text) {
    if (!text) return;
    const uint32_t seq = g_tail.fetch_add(1, std::memory_order_relaxed);
    Slot& slot = g_slots[seq % kLogRingSlots];

    // Overwrite semantics: if the consumer is more than a ring behind, we drop
    // this line and count it. Never block, never allocate.
    const uint32_t head = g_head.load(std::memory_order_acquire);
    if (seq - head >= kLogRingSlots) {
        g_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    slot.level = level;
    size_t n = std::strlen(text);
    if (n >= kLogLineMax) n = kLogLineMax - 1;
    std::memcpy(slot.text, text, n);
    slot.text[n] = '\0';
    slot.published.store(seq + 1, std::memory_order_release);
}

bool PopLog(Level& level, char* out, size_t cap) {
    for (;;) {
        const uint32_t head = g_head.load(std::memory_order_relaxed);
        if (head == g_tail.load(std::memory_order_acquire)) return false;
        Slot& slot = g_slots[head % kLogRingSlots];
        if (slot.published.load(std::memory_order_acquire) != head + 1) {
            // producer reserved but has not published yet: treat as empty for now
            return false;
        }
        level = slot.level;
        std::strncpy(out, slot.text, cap - 1);
        out[cap - 1] = '\0';
        slot.published.store(0, std::memory_order_relaxed);
        g_head.store(head + 1, std::memory_order_release);
        return true;
    }
}

size_t LogDropped() { return g_dropped.load(std::memory_order_relaxed); }

void LogLinef(Level level, const char* fmt, ...) {
    char buffer[kLogLineMax];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    LogLine(level, buffer);
}

bool ReadExact(void* handle, void* buffer, size_t n) {
    auto h = reinterpret_cast<HANDLE>(handle);
    auto* p = static_cast<uint8_t*>(buffer);
    size_t done = 0;
    while (done < n) {
        DWORD read = 0;
        if (!ReadFile(h, p + done, static_cast<DWORD>(n - done), &read, nullptr) || read == 0) return false;
        done += read;
    }
    return true;
}

bool WriteExact(void* handle, const void* buffer, size_t n) {
    auto h = reinterpret_cast<HANDLE>(handle);
    auto* p = static_cast<const uint8_t*>(buffer);
    size_t done = 0;
    while (done < n) {
        DWORD written = 0;
        if (!WriteFile(h, p + done, static_cast<DWORD>(n - done), &written, nullptr) || written == 0) return false;
        done += written;
    }
    return true;
}

}  // namespace phetamine::ipc
