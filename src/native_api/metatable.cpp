// native_api/metatable.cpp — raw metatables, metamethod hooks, readonly.
//
// Key correction to the draft spec: `lua_getmetatable` is ALREADY the raw read.
// It is Lua's `getmetatable()` builtin that honours `__metatable`; the C entry
// point returns whatever the metatable slot holds. So `getrawmetatable` needs no
// struct offsets — it is honest as implemented here.
//
// `hookmetamethod` is likewise real: read the original out of the raw metatable,
// write the replacement in (clearing readonly first when the fork lets us), and
// return the original so the caller can chain to it.
#include "native_api/api.h"
#include "lua/state.h"
#include "lua/layout.h"
#include "common/log.h"
#include "common/seh.h"

#include <unordered_map>
#include <string>

namespace phetamine::api {
namespace {

void Raise(lua_State* L, const char* message) {
    lua::Api& api = lua::GetApi();
    api.pushstring(L, message);
    if (api.error) api.error(L);
}

// readonly bookkeeping: the fork exposes a setter but no getter, so what we can
// answer truthfully is "what this session set". Anything else is unknown and
// says so instead of guessing.
std::unordered_map<const void*, bool> g_readonly;
bool g_warnedReadonly = false;

}  // namespace

int l_getrawmetatable(lua_State* L) {
    lua::Api& api = lua::GetApi();
    if (!api.getmetatable) {
        PushNil(L);
        return 1;
    }
    if (!api.getmetatable(L, 1)) {
        PushNil(L);
    }
    return 1;
}

int l_setrawmetatable(lua_State* L) {
    lua::Api& api = lua::GetApi();
    if (!api.setmetatable) {
        Raise(L, "setrawmetatable: lua_setmetatable is not resolved on this build");
        return 0;
    }
    if (api.type(L, 2) != lua::kTypeTable && api.type(L, 2) != lua::kTypeNil) {
        Raise(L, "setrawmetatable: table or nil expected");
        return 0;
    }
    // Clear readonly on the target first (docs: the fork has no getter, so the
    // table stays writable afterwards — that is the documented behaviour of
    // setrawmetatable in this executor, not a silent side effect).
    if (api.setreadonly && api.type(L, 1) == lua::kTypeTable) {
        api.pushvalue(L, 1);
        api.setreadonly(L, -1, 0);
        api.pop(L, 1);
    }
    api.pushvalue(L, 1);            // [obj]
    api.pushvalue(L, 2);            // [obj][mt]
    const int ok = api.setmetatable(L, -2);
    api.pop(L, 2);
    PushBool(L, ok != 0);
    return 1;
}

// hookmetamethod(obj, "__index", fn) → the original metamethod (or nil)
int l_hookmetamethod(lua_State* L) {
    lua::Api& api = lua::GetApi();
    const char* name = api.tostring ? api.tostring(L, 2) : nullptr;
    if (!name) {
        Raise(L, "hookmetamethod: metamethod name (string) expected");
        return 0;
    }
    if (api.type(L, 3) != lua::kTypeFunction) {
        Raise(L, "hookmetamethod: replacement function expected");
        return 0;
    }
    if (!api.getmetatable || !api.setfield || !api.getfield) {
        Raise(L, "hookmetamethod: the metatable API is not fully resolved on this build");
        return 0;
    }

    if (!api.getmetatable(L, 1)) {          // [mt]
        PushNil(L);
        return 1;
    }
    const int mtAbs = api.gettop(L);

    // Read the original before overwriting it.
    bool hadOriginal = api.getfield(L, mtAbs, name) != 0 && api.type(L, -1) != lua::kTypeNil;
    if (!hadOriginal) {
        api.pop(L, 1);                       // [mt]
    }                                        // else [mt][original]

    // Write our replacement into the raw metatable, bypassing readonly.
    if (api.setreadonly) {
        api.pushvalue(L, mtAbs);
        api.setreadonly(L, -1, 0);
        api.pop(L, 1);
    }
    api.pushvalue(L, mtAbs);                // [..][mt]
    api.pushvalue(L, 3);                    // [..][mt][fn]
    api.setfield(L, -2, name);              // [..][mt]
    api.pop(L, 1);                          // [..]

    if (!hadOriginal) PushNil(L);
    // stack now: [original or nil]
    return 1;
}

// getnamecallmethod() — reads the namecall TString off the thread. That field is
// configuration (lua/layout.h kNamecallTString); without it there is no honest
// answer, so this raises instead of returning a guess.
int l_getnamecallmethod(lua_State* L) {
    if (lua::layout::kNamecallTString == lua::layout::kUnset) {
        Raise(L, "getnamecallmethod: the namecall field is not configured for this build "
                 "(lua/layout.h) — refusing to guess");
        return 0;
    }
    uintptr_t tstring = 0;
    if (!seh::TryReadPtr(reinterpret_cast<uintptr_t>(L) + lua::layout::kNamecallTString, tstring) || !tstring) {
        PushNil(L);
        return 1;
    }
    // A TString carries its length and bytes inline in the fork; the string data
    // follows the header. Without a verified header layout we cannot decode it,
    // so this stays a documented gap rather than a fabricated name.
    Raise(L, "getnamecallmethod: TString decoding is not implemented for this revision");
    return 0;
}

int l_setnamecallmethod(lua_State* L) {
    (void)L;
    Raise(L, "setnamecallmethod: not available on this build (namecall field unresolved)");
    return 0;
}

int l_setreadonly(lua_State* L) {
    lua::Api& api = lua::GetApi();
    if (!api.setreadonly) {
        Raise(L, "setreadonly: lua_setreadonly is not resolved on this build");
        return 0;
    }
    const bool value = api.toboolean ? api.toboolean(L, 2) != 0 : false;
    api.pushvalue(L, 1);
    api.setreadonly(L, -1, value ? 1 : 0);
    if (api.topointer) {
        const void* key = api.topointer(L, -1);
        if (key) g_readonly[key] = value;
    }
    api.pop(L, 1);
    PushBool(L, true);
    return 1;
}

int l_isreadonly(lua_State* L) {
    lua::Api& api = lua::GetApi();
    if (api.topointer) {
        const void* key = api.topointer(L, 1);
        const auto it = g_readonly.find(key);
        if (it != g_readonly.end()) {
            PushBool(L, it->second);
            return 1;
        }
    }
    if (!g_warnedReadonly) {
        g_warnedReadonly = true;
        log::Info("isreadonly: the fork has no getter for the readonly flag, so only tables this "
                  "session set are reported; anything else answers false (documented)");
    }
    PushBool(L, false);
    return 1;
}

}  // namespace phetamine::api
