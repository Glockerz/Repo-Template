#include "lua/identity.h"
#include "lua/state.h"
#include "lua/layout.h"
#include "lua/threads.h"
#include "common/log.h"
#include "common/seh.h"

#include <cstdio>

namespace phetamine::lua::identity {
namespace {

bool g_roundTripPassed = false;
bool g_attempted = false;

inline bool configured() {
    if (layout::kExtraIdentity == layout::kUnset) return false;
    // Either the fork's own accessor is bound (preferred), or the lua_State
    // field offset has been transcribed for this revision.
    return GetApi().getthreaddata != nullptr || layout::kStateExtra != layout::kUnset;
}

// Resolves the extra-space pointer for a thread.
//
// Preferred route: the fork's own accessor `lua_getthreaddata(L)` (present in
// VM/include/lua.h). That is a VM call, so it is only used on threads we own or
// on the client main thread — never from a worker. Fallback: read the pointer
// field at the configured lua_State offset.
uintptr_t Extra(lua_State* L) {
    if (!L) return 0;
    Api& api = GetApi();
    if (api.getthreaddata) {
        void* data = nullptr;
        __try {
            data = api.getthreaddata(L);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            data = nullptr;
        }
        if (data) return reinterpret_cast<uintptr_t>(data);
    }
    if (layout::kStateExtra == layout::kUnset) return 0;
    uintptr_t extra = 0;
    if (!seh::TryReadPtr(reinterpret_cast<uintptr_t>(L) + layout::kStateExtra, extra)) return 0;
    return extra < 0x10000 ? 0 : extra;
}

}  // namespace

bool Resolved() { return g_roundTripPassed; }

uintptr_t Get(lua_State* L) {
    if (!g_roundTripPassed) return 0;
    const uintptr_t extra = Extra(L);
    if (!extra) return 0;
    if (layout::kExtraIdentityIsByte) {
        uint8_t value = 0;
        if (!seh::TryRead<uint8_t>(extra + layout::kExtraIdentity, value)) return 0;
        return value;
    }
    uintptr_t value = 0;
    if (!seh::TryReadPtr(extra + layout::kExtraIdentity, value)) return 0;
    return value;
}

bool Set(lua_State* L, uintptr_t identity) {
    if (!g_roundTripPassed || !L) return false;
    if (!threads::IsOurs(L)) {
        log::Warn("identity: refusing to write identity on a thread we do not own (%p)", (void*)L);
        return false;
    }
    const uintptr_t extra = Extra(L);
    if (!extra) return false;
    if (layout::kExtraIdentityIsByte) {
        if (!seh::TryWrite<uint8_t>(extra + layout::kExtraIdentity, static_cast<uint8_t>(identity))) return false;
    } else {
        if (!seh::TryWrite<uintptr_t>(extra + layout::kExtraIdentity, identity)) return false;
    }
    return Get(L) == identity;
}

uintptr_t GetCallingScript(lua_State* L) {
    if (!g_roundTripPassed || layout::kExtraScript == layout::kUnset) return 0;
    const uintptr_t extra = Extra(L);
    if (!extra) return 0;
    uintptr_t script = 0;
    if (!seh::TryReadPtr(extra + layout::kExtraScript, script)) return 0;
    return script < 0x10000 ? 0 : script;
}

std::string Describe() {
    if (!configured()) return "unconfigured (lua/layout.h)";
    char buf[96];
    std::snprintf(buf, sizeof(buf), "extra@lua_State+0x%llX, identity@extra+0x%llX (%s)",
                  static_cast<unsigned long long>(layout::kStateExtra),
                  static_cast<unsigned long long>(layout::kExtraIdentity),
                  layout::kExtraIdentityIsByte ? "byte" : "word");
    return buf;
}

bool ProbeRoundTrip(lua_State* main) {
    if (g_attempted) return g_roundTripPassed;
    g_attempted = true;

    if (!configured()) {
        log::Info("identity: not configured for this fork revision (%s) — getidentity/setidentity off",
                  Describe().c_str());
        return false;
    }

    Api& api = GetApi();
    if (!api.IsUsable() || !main) {
        log::Error("identity: cannot probe before the VM API is bound");
        return false;
    }

    lua_State* T = nullptr;
    __try {
        T = api.newthread(main);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        T = nullptr;
    }
    if (!T) {
        log::Error("identity: scratch thread creation failed — identity stays off");
        return false;
    }

    threads::Add(T);
    bool ok = false;
    const uintptr_t before = Get(T);
    if (Set(T, kElevated)) {
        const uintptr_t after = Get(T);
        ok = (after == kElevated);
        if (!ok) {
            log::Error("identity: round-trip mismatch (wrote 8, read %llu) — offset %s is wrong; "
                       "disabling identity instead of writing a wrong field",
                       static_cast<unsigned long long>(after), Describe().c_str());
        }
    } else {
        log::Error("identity: round-trip write failed — identity off");
    }

    // Retire the scratch thread regardless of outcome; never leave it live.
    if (api.resetthread) {
        __try { api.resetthread(T); } __except (EXCEPTION_EXECUTE_HANDLER) { /* best effort */ }
    }
    threads::Remove(T);

    g_roundTripPassed = ok;
    if (ok) {
        log::Info("identity: round-trip OK (%s), identity 8 → read back 8 (scratch thread had %llu before)",
                  Describe().c_str(), static_cast<unsigned long long>(before));
    }
    return ok;
}

}  // namespace phetamine::lua::identity
