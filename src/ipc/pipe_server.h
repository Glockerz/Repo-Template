// ipc/pipe_server.h — \\.\pipe\PHETAMINE_<pid>
//
// One instance, one client (the UI). Two threads:
//   * reader  — parses frames and dispatches; NEVER touches the VM, it only
//               compiles (worker-safe) and enqueues scheduler jobs;
//   * writer  — drains the log ring and pushes frames; also serialises
//               ExecResult frames coming back from the main thread.
//
// The pipe name is keyed by PID so the UI can attach to an already-injected
// process after a restart instead of injecting again.
#pragma once

#include <windows.h>
#include <cstdint>
#include <string>
#include "ipc/protocol.h"

namespace phetamine::ipc {

struct Info {
    uint64_t placeId = 0;
    uint64_t gameId = 0;
    uint64_t userId = 0;
    std::string jobId;
    std::string displayName;
    std::string clientVersion;
};

// Starts the server. Returns false when the pipe cannot be created (the UI then
// reports a failed attach instead of hanging).
bool Start(uint32_t pid);

// Waits for the UI to connect. Called from the init pipeline (a worker thread),
// never from the drain. Returns false on error.
bool AcceptOne();

void Stop();

bool Running();
const std::wstring& PipeName();

// Sends a frame to the connected UI. Safe to call from any thread; frames are
// serialised through the writer lock. `ok=false` when no client is connected.
bool SendFrame(Op op, const void* payload, uint32_t length);
bool SendText(Op op, const char* text);

// Completion of a job (called from the drain, which must stay allocation-light:
// the payload is formatted into a stack buffer).
void PostExecResult(uint64_t jobId, bool ok, const char* message);
void PostReady(const Info& info);
void PostCanary(const char* json);

// Requests a clean shutdown of the whole module (posted to core).
void RequestUnload();
bool UnloadRequested();

}  // namespace phetamine::ipc
