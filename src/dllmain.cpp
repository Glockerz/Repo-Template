// dllmain.cpp — the image's two entry points, and nothing else.
//
//   1. DllMain: the normal-load path (LoadLibrary, used by the dev/test path).
//      It does exactly two things — DisableThreadLibraryCalls and hand off to the
//      same `inject::Start` the shellcode stub calls — because everything else
//      would run under the loader lock.
//
//   2. PhetamineEntry: the only exported symbol. The stub parses the mapped
//      image's export directory for this name after it has mapped, relocated and
//      fixed up the image, and calls it with the param block. Keeping the entry
//      here (next to DllMain) rather than in loader.cpp means there is one file to
//      look at when asking "how does this DLL start?".
#include "inject/loader.h"

#include <windows.h>

extern "C" __declspec(dllexport) void __stdcall PhetamineEntry(
    const phetamine::inject::ParamBlock* params, void* loaderBase, void* imageBase) {
    phetamine::inject::Start(params, reinterpret_cast<uintptr_t>(loaderBase),
                             reinterpret_cast<uintptr_t>(imageBase));
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        // Our threads are created deliberately, so the per-thread notifications
        // are noise — and avoiding them means never touching the loader lock.
        DisableThreadLibraryCalls(instance);
        phetamine::inject::Start(nullptr, 0, reinterpret_cast<uintptr_t>(instance));
    }
    // Nothing to do on detach: core::Shutdown has already stopped every thread,
    // released the environment and (for a manual map) unmapped the image from a
    // page outside itself.
    return TRUE;
}
