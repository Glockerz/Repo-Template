// ipc/protocol.h — the UI ↔ DLL wire format.
//
// Framing:  uint32 length | uint8 opcode | payload[length]
//           `length` counts the payload only. Little-endian, as both ends are.
//
// The opcode table is an X-macro so that the C# side can be checked against it
// mechanically (tools/verify.mjs check 4: name *and* value must agree, because a
// mismatch produces a pipe that connects and then silently misbehaves).
//
// Payloads are deliberately binary and flat:
//   Execute      : [u32 id][utf8 source]
//   ExecResult   : [u32 id][u8 ok][utf8 message]
//   Ready/Info   : [utf8 json]  (human-facing status blob, parsed by the UI)
//   Log          : [u8 level][utf8 line]
//   Canary       : [utf8 json]  (capability bitmap)
//   Capabilities : [utf8 json]
#pragma once

#include <cstdint>
#include <cstddef>

#define PHETAMINE_OPCODES(X) \
    /* direction: S→U = DLL to UI, U→S = UI to DLL */ \
    X(0x01, Ready)        /* S→U: handshake complete  */ \
    X(0x02, Info)         /* S→U: placeId/jobId/user   */ \
    X(0x03, Log)          /* S→U: script + engine log  */ \
    X(0x04, Canary)       /* S→U: capability bitmap    */ \
    X(0x05, ExecResult)   /* S→U: {id, ok, message}    */ \
    X(0x10, Execute)      /* U→S: run source           */ \
    X(0x11, Stop)         /* U→S: retire script threads*/ \
    X(0x12, Unload)       /* U→S: clean shutdown       */ \
    X(0x13, Ping)         /* U→S: liveness probe       */ \
    X(0x14, Capabilities) /* U→S: request the bitmap   */

namespace phetamine::ipc {

enum class Op : uint8_t {
#define X(value, name) name = value,
    PHETAMINE_OPCODES(X)
#undef X
};

const char* OpName(Op op);

// The longest payload we will read or write in one frame.
inline constexpr uint32_t kMaxFrame = 8u << 20;   // 8 MiB: a very large script

// ---- log ring ----------------------------------------------------------------
// Fixed-size, lock-free, no allocation: the scheduler drain and the Lua C
// closures push here, and the pipe writer thread drains it. A full ring drops
// the oldest-newest and counts the loss rather than blocking the caller.
enum class Level : uint8_t { Debug = 0, Info = 1, Warn = 2, Error = 3 };

inline constexpr size_t kLogLineMax = 900;    // fits the 1024-byte slot with header
inline constexpr size_t kLogRingSlots = 2048;

void   LogLine(Level level, const char* text);          // producer, any thread
bool   PopLog(Level& level, char* out, size_t cap);     // consumer: pipe writer
size_t LogDropped();
void   LogLinef(Level level, const char* fmt, ...);     // formats into a stack buffer

// ---- helpers used by both ends ----------------------------------------------
// Reads exactly `n` bytes; false on EOF/short read.
bool ReadExact(void* handle, void* buffer, size_t n);
bool WriteExact(void* handle, const void* buffer, size_t n);

}  // namespace phetamine::ipc
