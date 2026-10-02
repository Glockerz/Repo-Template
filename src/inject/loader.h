// inject/loader.h — everything that exists because this image may have been
// mapped by our own shellcode rather than by the Windows loader.
//
// Two loaders can start us:
//
//   1. the shellcode stub (inject/stub/stub.c) — maps the image by hand and calls
//      `PhetamineEntry` with the param block. In this mode there is no PEB
//      module entry, `FreeLibrary` is meaningless, and shutdown must unmap the
//      image itself.
//   2. the Windows loader (LoadLibrary, used by the dev/test path) — DllMain runs
//      with default parameters and shutdown only stops our threads.
//
// The exported entry is also the only symbol the stub has to find, so it must
// keep the exact name `PhetamineEntry` and the `extern "C"` signature below.
#pragma once

#include <cstdint>
#include "inject/param.h"

namespace phetamine::inject {

// Called by the stub (and by DllMain in the normal-load case).
// `loaderBase` is the stub's own mapping, or 0 when nobody knows it.
void Start(const ParamBlock* params, uintptr_t loaderBase, uintptr_t imageBase);

// True when the stub mapped us: changes shutdown behaviour and makes
// `GetModuleHandle(nullptr)`-style lookups of our own image pointless.
bool ManualMap();

const ParamBlock* Parameters();
const wchar_t* WorkspaceFromParams();      // nullptr when the block carried none

// ---- main-thread entry -------------------------------------------------------
// Runs `fn(arg)` on the client's main thread and returns true when it ran. The
// primary mechanism is a one-shot WH_GETMESSAGE hook on the thread that owns the
// game window: the OS calls us while that thread is pumping messages, which is a
// point at which the VM is idle and a Luau call is legal. The APC path
// (scheduler/rendezvous.cpp) stays off by default — many threads never reach an
// alertable wait.
//
// Failure is honest: returns false, and the caller fails init with
// ERROR:Bootstrap instead of pretending scripts will run.
bool RequestMainThreadBootstrap(bool (*fn)(void*), void* arg, uint32_t timeoutMs = 5000);

// The id of the thread that owns the client window (0 when none was found).
uint32_t MainThreadId();

// ---- shutdown ----------------------------------------------------------------
// Unmaps this image when ManualMap() is true, then terminates the calling thread
// from a stub page outside the image (the FreeLibraryAndExitThread pattern, hand
// rolled because a manual map has no module table). No-op otherwise.
void SelfUnmapIfManual();

}  // namespace phetamine::inject
