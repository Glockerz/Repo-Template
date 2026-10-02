// native_api/identity.cpp — thread identity.
//
// The rule from ADR-8: identity is applied to threads WE created, and
// `setidentity` refuses anything else. If the fork's `lua_getthreaddata` or the
// configured extra-space offsets cannot be validated by a round-trip, the
// capability is off and these functions are never registered — a script calling
// setidentity() on such a build gets `attempt to call a nil value`, which is
// strictly better than a silent write into a guessed field.
#include "native_api/api.h"
#include "lua/state.h"
#include "lua/identity.h"
#include "lua/threads.h"
#include "executor/executor.h"
#include "common/log.h"

namespace phetamine::api {
namespace {

void Raise(lua_State* L, const char* message) {
    lua::Api& api = lua::GetApi();
    api.pushstring(L, message);
    if (api.error) api.error(L);
}

// Thread argument: the calling thread when absent, the actual thread when the
// caller passed one (lua_tothread), nullptr when the value is not a thread.
lua_State* ThreadArg(lua_State* L, int index) {
    lua::Api& api = lua::GetApi();
    if (api.type(L, index) == lua::kTypeNone || api.type(L, index) == lua::kTypeNil) return L;
    if (api.type(L, index) != lua::kTypeThread || !api.tothread) return nullptr;
    return api.tothread(L, index);
}

}  // namespace

int l_getidentity(lua_State* L) {
    PushNumber(L, static_cast<double>(lua::identity::Get(L)));
    return 1;
}

int l_setidentity(lua_State* L) {
    lua::Api& api = lua::GetApi();
    const uintptr_t value = static_cast<uintptr_t>(api.tointeger(L, 1, nullptr));
    if (!lua::identity::Resolved()) {
        Raise(L, "setidentity: thread identity is unavailable on this build "
                 "(the extra-space round-trip probe did not pass)");
        return 0;
    }
    // The calling thread is the script thread, so it is ours by construction.
    if (!lua::identity::Set(L, value)) {
        Raise(L, "setidentity: the identity write did not round-trip — refusing");
        return 0;
    }
    PushBool(L, true);
    return 1;
}

int l_getthreadidentity(lua_State* L) {
    lua_State* target = ThreadArg(L, 1);
    if (!target) {
        Raise(L, "getthreadidentity: argument is not a thread");
        return 0;
    }
    PushNumber(L, static_cast<double>(lua::identity::Get(target)));
    return 1;
}

int l_setthreadidentity(lua_State* L) {
    lua::Api& api = lua::GetApi();
    const uintptr_t value = static_cast<uintptr_t>(api.tointeger(L, 1, nullptr));
    if (!lua::identity::Resolved()) {
        Raise(L, "setthreadidentity: thread identity is unavailable on this build");
        return 0;
    }
    lua_State* target = ThreadArg(L, 2);
    if (!target) {
        Raise(L, "setthreadidentity: second argument must be a thread");
        return 0;
    }
    // ADR-8: only threads this executor created may be re-identified. An engine
    // thread's identity is engine state, and changing it is how a "feature"
    // becomes a crash the user cannot attribute.
    if (!lua::threads::IsOurs(target)) {
        Raise(L, "setthreadidentity: refusing to write identity on an engine thread");
        return 0;
    }
    if (!lua::identity::Set(target, value)) {
        Raise(L, "setthreadidentity: the identity write did not round-trip — refusing");
        return 0;
    }
    PushBool(L, true);
    return 1;
}

// getthreadcontext([thread]) → { identity = n, caller = bool, current = bool }
int l_getthreadcontext(lua_State* L) {
    lua::Api& api = lua::GetApi();
    lua_State* target = ThreadArg(L, 1);
    if (!target) {
        Raise(L, "getthreadcontext: argument is not a thread");
        return 0;
    }
    api.createtable(L, 0, 3);
    PushNumber(L, static_cast<double>(lua::identity::Get(target)));
    api.setfield(L, -2, "identity");
    PushBool(L, lua::threads::IsOurs(target));
    api.setfield(L, -2, "caller");
    PushBool(L, exec::CurrentScriptThread() == target);
    api.setfield(L, -2, "current");
    return 1;
}

}  // namespace phetamine::api
