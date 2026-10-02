#include "ipc/pipe_server.h"
#include "scheduler/scheduler.h"
#include "executor/executor.h"
#include "native_api/api.h"
#include "common/log.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace phetamine::ipc {
namespace {

HANDLE g_pipe = INVALID_HANDLE_VALUE;
std::wstring g_pipeName;
std::atomic<bool> g_running{ false };
std::atomic<bool> g_unload{ false };
std::atomic<bool> g_connected{ false };

HANDLE g_readerThread = nullptr;
HANDLE g_writerThread = nullptr;
SRWLOCK g_writeLock = SRWLOCK_INIT;

// UI id → job id mapping, so ExecResult can echo the id the UI sent.
struct Pending {
    uint32_t uiId = 0;
    uint64_t jobId = 0;
};
constexpr size_t kMaxPending = 128;
Pending g_pending[kMaxPending];
SRWLOCK g_pendingLock = SRWLOCK_INIT;

void RememberPending(uint32_t uiId, uint64_t jobId) {
    AcquireSRWLockExclusive(&g_pendingLock);
    for (auto& p : g_pending) {
        if (p.uiId == 0) { p.uiId = uiId; p.jobId = jobId; break; }
    }
    ReleaseSRWLockExclusive(&g_pendingLock);
}

uint32_t TakeUiId(uint64_t jobId) {
    uint32_t found = 0;
    AcquireSRWLockExclusive(&g_pendingLock);
    for (auto& p : g_pending) {
        if (p.jobId == jobId) {
            found = p.uiId;
            p.uiId = 0;
            p.jobId = 0;
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_pendingLock);
    return found;
}

bool RawSend(Op op, const void* payload, uint32_t length) {
    if (g_pipe == INVALID_HANDLE_VALUE || !g_connected.load()) return false;
    const uint32_t header = length;
    AcquireSRWLockExclusive(&g_writeLock);
    bool ok = WriteExact(g_pipe, &header, sizeof(header));
    if (ok) {
        const uint8_t opByte = static_cast<uint8_t>(op);
        ok = WriteExact(g_pipe, &opByte, 1);
    }
    if (ok && length) ok = WriteExact(g_pipe, payload, length);
    if (!ok) {
        g_connected.store(false);
        log::Warn("ipc: write failed (%lu) — marking the client disconnected", GetLastError());
    }
    ReleaseSRWLockExclusive(&g_writeLock);
    return ok;
}

// ---- reader ------------------------------------------------------------------

void Dispatch(Op op, const std::vector<uint8_t>& payload) {
    switch (op) {
        case Op::Execute: {
            if (payload.size() < 4) {
                SendText(Op::ExecResult, "{\"ok\":false,\"message\":\"malformed EXECUTE\"}");
                return;
            }
            uint32_t uiId = 0;
            std::memcpy(&uiId, payload.data(), 4);
            const std::string source(reinterpret_cast<const char*>(payload.data() + 4), payload.size() - 4);

            std::string error;
            const uint64_t jobId = exec::Run(source, error);   // compile happens here (worker-safe)
            if (jobId == 0) {
                char buf[1024];
                std::snprintf(buf, sizeof(buf), "{\"id\":%u,\"ok\":false,\"message\":\"%s\"}", uiId, error.c_str());
                SendText(Op::ExecResult, buf);
                return;
            }
            RememberPending(uiId, jobId);
            char buf[256];
            std::snprintf(buf, sizeof(buf), "{\"id\":%u,\"ok\":true,\"queued\":%llu}", uiId,
                          static_cast<unsigned long long>(jobId));
            SendText(Op::ExecResult, buf);
            return;
        }
        case Op::Stop: {
            sched::Job job{};
            job.kind = sched::JobKind::StopScripts;
            sched::Enqueue(job);
            SendText(Op::ExecResult, "{\"ok\":true,\"message\":\"stop queued\"}");
            return;
        }
        case Op::Unload: {
            g_unload.store(true);
            sched::Job job{};
            job.kind = sched::JobKind::Shutdown;
            sched::Enqueue(job);
            SendText(Op::ExecResult, "{\"ok\":true,\"message\":\"unloading\"}");
            return;
        }
        case Op::Ping:
            SendText(Op::Info, "{\"pong\":true}");
            return;
        case Op::Capabilities: {
            char json[4096];
            api::CapabilitiesJson(json, sizeof(json));
            SendText(Op::Capabilities, json);
            return;
        }
        default:
            log::Warn("ipc: unknown opcode 0x%02X — ignoring (protocol mismatch?)", static_cast<unsigned>(op));
            return;
    }
}

DWORD WINAPI ReaderProc(LPVOID) {
    std::vector<uint8_t> payload;
    while (g_running.load()) {
        uint32_t length = 0;
        if (!ReadExact(g_pipe, &length, sizeof(length))) break;
        if (length > kMaxFrame) {
            log::Error("ipc: frame of %u bytes exceeds the %u cap — dropping the connection", length, kMaxFrame);
            break;
        }
        uint8_t opByte = 0;
        if (!ReadExact(g_pipe, &opByte, 1)) break;
        payload.resize(length);
        if (length && !ReadExact(g_pipe, payload.data(), length)) break;
        Dispatch(static_cast<Op>(opByte), payload);
    }
    g_connected.store(false);
    log::Info("ipc: reader stopped");
    return 0;
}

DWORD WINAPI WriterProc(LPVOID) {
    while (g_running.load()) {
        Level level = Level::Info;
        char line[kLogLineMax];
        bool sent = false;
        while (PopLog(level, line, sizeof(line))) {
            uint8_t buffer[1 + kLogLineMax];
            buffer[0] = static_cast<uint8_t>(level);
            const size_t n = std::strlen(line);
            std::memcpy(buffer + 1, line, n);
            RawSend(Op::Log, buffer, static_cast<uint32_t>(1 + n));
            sent = true;
        }
        if (!sent) Sleep(15);
    }
    return 0;
}

}  // namespace

bool Start(uint32_t pid) {
    wchar_t name[128];
    std::swprintf(name, 128, L"\\\\.\\pipe\\PHETAMINE_%u", pid);
    g_pipeName = name;

    g_pipe = CreateNamedPipeW(
        g_pipeName.c_str(),
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1,                 // one UI
        1 << 16, 1 << 16,
        0,
        nullptr);
    if (g_pipe == INVALID_HANDLE_VALUE) {
        log::StageError("IPC", "CreateNamedPipeW(%ls) failed: %lu", g_pipeName.c_str(), GetLastError());
        return false;
    }
    g_running.store(true);

    g_writerThread = CreateThread(nullptr, 0, WriterProc, nullptr, 0, nullptr);
    log::Info("ipc: waiting for the UI on %ls", g_pipeName.c_str());
    return true;
}

// Accepts exactly one connection (called from the init pipeline, on a worker).
bool AcceptOne() {
    if (g_pipe == INVALID_HANDLE_VALUE) return false;
    if (!ConnectNamedPipe(g_pipe, nullptr)) {
        const DWORD err = GetLastError();
        if (err != ERROR_PIPE_CONNECTED) {
            log::StageError("IPC", "ConnectNamedPipe failed: %lu", err);
            return false;
        }
    }
    g_connected.store(true);
    g_readerThread = CreateThread(nullptr, 0, ReaderProc, nullptr, 0, nullptr);
    log::Info("ipc: UI connected");
    return true;
}

void Stop() {
    g_running.store(false);
    g_connected.store(false);
    if (g_pipe != INVALID_HANDLE_VALUE) {
        // Closing the handle unblocks the reader's pending ReadFile.
        HANDLE pipe = g_pipe;
        g_pipe = INVALID_HANDLE_VALUE;
        CancelIoEx(pipe, nullptr);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
    if (g_readerThread) { WaitForSingleObject(g_readerThread, 2000); CloseHandle(g_readerThread); g_readerThread = nullptr; }
    if (g_writerThread) { WaitForSingleObject(g_writerThread, 2000); CloseHandle(g_writerThread); g_writerThread = nullptr; }
    log::Info("ipc: stopped (%zu log line(s) dropped)", LogDropped());
}

bool Running() { return g_running.load(); }
const std::wstring& PipeName() { return g_pipeName; }

bool SendFrame(Op op, const void* payload, uint32_t length) { return RawSend(op, payload, length); }

bool SendText(Op op, const char* text) {
    if (!text) return false;
    return RawSend(op, text, static_cast<uint32_t>(std::strlen(text)));
}

void PostExecResult(uint64_t jobId, bool ok, const char* message) {
    char buffer[1024];
    const uint32_t uiId = TakeUiId(jobId);
    std::snprintf(buffer, sizeof(buffer), "{\"id\":%u,\"job\":%llu,\"ok\":%s,\"message\":\"%s\"}",
                  uiId, static_cast<unsigned long long>(jobId), ok ? "true" : "false",
                  message ? message : "");
    SendText(Op::ExecResult, buffer);
}

void PostReady(const Info& info) {
    char buffer[1024];
    std::snprintf(buffer, sizeof(buffer),
                  "{\"placeId\":%llu,\"gameId\":%llu,\"userId\":%llu,\"jobId\":\"%s\","
                  "\"displayName\":\"%s\",\"clientVersion\":\"%s\",\"drain\":%s}",
                  static_cast<unsigned long long>(info.placeId),
                  static_cast<unsigned long long>(info.gameId),
                  static_cast<unsigned long long>(info.userId),
                  info.jobId.c_str(), info.displayName.c_str(), info.clientVersion.c_str(),
                  sched::HeartbeatFresh() ? "true" : "false");
    SendText(Op::Ready, buffer);
}

void PostCanary(const char* json) { SendText(Op::Canary, json); }

void RequestUnload() { g_unload.store(true); }
bool UnloadRequested() { return g_unload.load(); }

}  // namespace phetamine::ipc
