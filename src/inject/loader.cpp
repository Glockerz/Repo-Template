#include "inject/loader.h"
#include "core/core.h"
#include "common/log.h"
#include "common/seh.h"

#include <windows.h>
#include <atomic>
#include <cstring>
#include <vector>

namespace phetamine::inject {
namespace {

std::atomic<bool> g_started{ false };
std::atomic<bool> g_manualMap{ false };
uintptr_t g_imageBase = 0;
uintptr_t g_loaderBase = 0;
ParamBlock g_params{};
bool g_haveParams = false;

DWORD WINAPI InitProc(LPVOID) {
    core::Initialize();
    return 0;
}

// ---- main-thread lookup ------------------------------------------------------

struct WindowSearch {
    DWORD pid = 0;
    HWND best = nullptr;
    LONG bestArea = 0;
};

BOOL CALLBACK EnumProc(HWND window, LPARAM param) {
    WindowSearch* search = reinterpret_cast<WindowSearch*>(param);
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid != search->pid) return TRUE;
    if (!IsWindowVisible(window)) return TRUE;

    RECT rect{};
    if (!GetClientRect(window, &rect)) return TRUE;
    const LONG area = (rect.right - rect.left) * (rect.bottom - rect.top);
    // A top-level window with no owner is the game window; popups lose.
    const bool topLevel = GetWindow(window, GW_OWNER) == nullptr;
    if (topLevel && area >= search->bestArea) {
        search->bestArea = area;
        search->best = window;
    }
    return TRUE;
}

DWORD ThreadIdFromWindow() {
    WindowSearch search{};
    search.pid = GetCurrentProcessId();
    EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&search));
    if (!search.best) return 0;
    return GetWindowThreadProcessId(search.best, nullptr);
}

DWORD ThreadIdFromSnapshot() {
    // Fallback: the main thread is the oldest thread in the process. This is a
    // weaker signal than a window, so it is only used when EnumWindows found
    // nothing (the client is starting up, or headless).
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    DWORD oldest = 0;
    ULONGLONG oldestTime = ~0ull;
    if (Thread32First(snapshot, &entry)) {
        do {
            if (entry.th32OwnerProcessID != GetCurrentProcessId()) continue;
            HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
            if (!thread) continue;
            FILETIME created{}, exit{}, kernel{}, user{};
            if (GetThreadTimes(thread, &created, &exit, &kernel, &user)) {
                ULARGE_INTEGER stamp{};
                stamp.LowPart = created.dwLowDateTime;
                stamp.HighPart = created.dwHighDateTime;
                if (stamp.QuadPart < oldestTime) {
                    oldestTime = stamp.QuadPart;
                    oldest = entry.th32ThreadID;
                }
            }
            CloseHandle(thread);
        } while (Thread32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return oldest;
}

std::atomic<DWORD> g_mainThread{ 0 };

struct Pending {
    bool (*fn)(void*) = nullptr;
    void* arg = nullptr;
    std::atomic<bool> claimed{ false };
    bool ok = false;
    HANDLE done = nullptr;
};

Pending* volatile g_pending = nullptr;
HHOOK g_hook = nullptr;

LRESULT CALLBACK HookProc(int code, WPARAM wParam, LPARAM lParam) {
    // Runs inside the client's message pump — i.e. on the main thread, at a point
    // where the VM is idle. This is the whole point of the hook.
    Pending* pending = g_pending;
    if (code >= 0 && pending && !pending->claimed.exchange(true)) {
        __try {
            pending->ok = pending->fn(pending->arg);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            log::Error("inject: the bootstrap callback faulted");
            pending->ok = false;
        }
        if (g_hook) UnhookWindowsHookEx(g_hook);   // no second call
        if (pending->done) SetEvent(pending->done);
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

// ---- self-unmap trampoline ---------------------------------------------------
//
// A manual map has no entry in the process's module table, so nothing can call
// FreeLibrary for us. The image therefore unmaps itself from a page OUTSIDE the
// image, then terminates its threads from ntdll. This runs at most once, at the
// very end of Shutdown(), after every thread we own has stopped.

void Emit(uint8_t*& cursor, const void* bytes, size_t length) {
    std::memcpy(cursor, bytes, length);
    cursor += length;
}

void EmitU8(uint8_t*& cursor, uint8_t value) { *cursor++ = value; }

void Emit64(uint8_t*& cursor, uint64_t value) {
    std::memcpy(cursor, &value, sizeof(value));
    cursor += sizeof(value);
}

constexpr uint8_t kMovRcxImm64[]  = { 0x48, 0xB9 };
constexpr uint8_t kMovRdxImm64[]  = { 0x48, 0xBA };
constexpr uint8_t kMovRaxImm64[]  = { 0x48, 0xB8 };
constexpr uint8_t kCallRax[]      = { 0xFF, 0xD0 };
constexpr uint8_t kSubRsp28[]     = { 0x48, 0x83, 0xEC, 0x28 };
constexpr uint8_t kAddRsp28[]     = { 0x48, 0x83, 0xC4, 0x28 };
constexpr uint8_t kMovRcxMinus1[] = { 0x48, 0xC7, 0xC1, 0xFF, 0xFF, 0xFF, 0xFF };
constexpr uint8_t kXorRdx[]       = { 0x48, 0x31, 0xD2 };
constexpr uint8_t kMovRcxMinus2[] = { 0x48, 0xC7, 0xC1, 0xFE, 0xFF, 0xFF, 0xFF };

DWORD WINAPI TrampolineProc(LPVOID) {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return 1;
    auto unmap = reinterpret_cast<uint64_t>(GetProcAddress(ntdll, "NtUnmapViewOfSection"));
    auto terminate = reinterpret_cast<uint64_t>(GetProcAddress(ntdll, "NtTerminateThread"));
    if (!unmap || !terminate) return 1;

    HANDLE original = OpenThread(THREAD_TERMINATE, FALSE, GetCurrentThreadId());
    if (!original) return 1;

    uint8_t* page = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE,
                                                       PAGE_READWRITE));
    if (!page) return 1;

    uint8_t* cursor = page;
    Emit(cursor, kSubRsp28, sizeof(kSubRsp28));

    // NtTerminateThread(the thread that is calling us now, 0)
    Emit(cursor, kMovRcxImm64, sizeof(kMovRcxImm64));
    Emit64(cursor, reinterpret_cast<uint64_t>(original));
    Emit(cursor, kMovRaxImm64, sizeof(kMovRaxImm64));
    Emit64(cursor, terminate);
    Emit(cursor, kXorRdx, sizeof(kXorRdx));
    Emit(cursor, kCallRax, sizeof(kCallRax));

    // NtUnmapViewOfSection((HANDLE)-1, imageBase)
    Emit(cursor, kMovRcxMinus1, sizeof(kMovRcxMinus1));
    Emit(cursor, kMovRdxImm64, sizeof(kMovRdxImm64));
    Emit64(cursor, g_imageBase);
    Emit(cursor, kMovRaxImm64, sizeof(kMovRaxImm64));
    Emit64(cursor, unmap);
    Emit(cursor, kCallRax, sizeof(kCallRax));

    // NtTerminateThread((HANDLE)-2, 0) — this thread, now that its image is gone.
    Emit(cursor, kMovRcxMinus2, sizeof(kMovRcxMinus2));
    Emit(cursor, kMovRaxImm64, sizeof(kMovRaxImm64));
    Emit64(cursor, terminate);
    Emit(cursor, kXorRdx, sizeof(kXorRdx));
    Emit(cursor, kCallRax, sizeof(kCallRax));
    EmitU8(cursor, 0xCC);

    DWORD previous = 0;
    if (!VirtualProtect(page, 4096, PAGE_EXECUTE_READ, &previous)) {
        VirtualFree(page, 0, MEM_RELEASE);
        return 1;
    }
    FlushInstructionCache(GetCurrentProcess(), page, 4096);

    DWORD threadId = 0;
    HANDLE thread = CreateThread(nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(page),
                                 nullptr, 0, &threadId);
    if (!thread) return 1;
    CloseHandle(thread);
    // Do not return into the image: it is about to disappear. The trampoline
    // terminates this thread; if it somehow fails we stay parked in the kernel.
    Sleep(INFINITE);
    return 0;
}

}  // namespace

DWORD MainThreadId() {
    DWORD cached = g_mainThread.load();
    if (cached) return cached;
    DWORD threadId = ThreadIdFromWindow();
    if (!threadId) threadId = ThreadIdFromSnapshot();
    if (threadId) g_mainThread.store(threadId);
    return threadId;
}

bool RequestMainThreadBootstrap(bool (*fn)(void*), void* arg, uint32_t timeoutMs) {
    if (!fn) return false;

    const DWORD mainThread = MainThreadId();
    if (!mainThread) {
        log::Error("inject: no main thread found — the client has no window and no thread to hook");
        return false;
    }
    if (mainThread == GetCurrentThreadId()) {
        // Already there (the normal-load path can be started from the main
        // thread): direct call, no hook, no window to wait for.
        return fn(arg);
    }
    if (g_pending) {
        log::Error("inject: a bootstrap is already pending — refusing to nest them");
        return false;
    }

    Pending pending{};
    pending.fn = fn;
    pending.arg = arg;
    pending.done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!pending.done) return false;

    g_pending = &pending;
    // hMod = NULL is the documented form for a thread-specific hook whose
    // procedure lives in this process — and it is the only form that works for a
    // manually mapped image (there is no module handle to pass).
    g_hook = SetWindowsHookExW(WH_GETMESSAGE, HookProc, nullptr, mainThread);
    if (!g_hook) {
        log::Error("inject: SetWindowsHookExW(WH_GETMESSAGE) failed: %lu", GetLastError());
        g_pending = nullptr;
        CloseHandle(pending.done);
        return false;
    }

    // Any message wakes the pump and delivers the hook; a posted WM_NULL is the
    // "no traffic right now" case.
    PostThreadMessageW(mainThread, WM_NULL, 0, 0);

    const DWORD waitResult = WaitForSingleObject(pending.done, timeoutMs);
    if (waitResult != WAIT_OBJECT_0) {
        log::Error("inject: the main thread did not reach the hook within %u ms", timeoutMs);
    }
    if (g_hook) {
        UnhookWindowsHookEx(g_hook);
        g_hook = nullptr;
    }
    g_pending = nullptr;
    CloseHandle(pending.done);

    // A timeout is not "maybe it ran": `claimed` is set before fn is called, so
    // this answers exactly whether the main thread executed the bootstrap.
    return (waitResult == WAIT_OBJECT_0) && pending.claimed.load() && pending.ok;
}

void Start(const ParamBlock* params, uintptr_t loaderBase, uintptr_t imageBase) {
    if (g_started.exchange(true)) return;
    g_imageBase = imageBase;
    g_loaderBase = loaderBase;
    if (Validate(params)) {
        // Copy: the loader may free its param block as soon as we return.
        std::memcpy(&g_params, params, sizeof(ParamBlock));
        g_haveParams = true;
    }
    g_manualMap.store(g_haveParams && (g_params.flags & kFlagManualMap) != 0);

    log::Info("inject: start (manual map: %s, loader base 0x%llX, image 0x%llX)",
              ManualMap() ? "yes" : "no", static_cast<unsigned long long>(g_loaderBase),
              static_cast<unsigned long long>(g_imageBase));

    HANDLE thread = CreateThread(nullptr, 0, InitProc, nullptr, 0, nullptr);
    if (!thread) {
        log::Fatal("inject: CreateThread failed: %lu — nothing can start", GetLastError());
        return;
    }
    CloseHandle(thread);
}

bool ManualMap() { return g_manualMap.load(); }
const ParamBlock* Parameters() { return g_haveParams ? &g_params : nullptr; }

const wchar_t* WorkspaceFromParams() {
    if (!g_haveParams) return nullptr;
    return g_params.workspace[0] ? g_params.workspace : nullptr;
}

void SelfUnmapIfManual() {
    if (!ManualMap()) return;
    if (!g_imageBase) return;
    log::Info("inject: unmapping this image (manual map) and terminating the thread");
    TrampolineProc(nullptr);
}

}  // namespace phetamine::inject

