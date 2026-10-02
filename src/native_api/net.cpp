// native_api/net.cpp — request() and friends, straight over WinHTTP.
//
// Threading contract: the synchronous functions run the HTTP call on a WORKER
// (never on the drain thread — a network stall must not hitch a frame), and the
// async variants hand their result back to Lua through a scheduler CallInto job,
// which runs the callback on the main thread at a legal point.
#include "native_api/api.h"
#include "net/client.h"
#include "lua/state.h"
#include "scheduler/scheduler.h"
#include "common/log.h"

#include <string>
#include <thread>
#include <vector>

namespace phetamine::api {
namespace {

void Raise(lua_State* L, const char* message) {
    lua::Api& api = lua::GetApi();
    api.pushstring(L, message);
    if (api.error) api.error(L);
}

// Reads the request table: { Url|url, Method|method, Headers|headers, Body|body }
bool ReadRequest(lua_State* L, int index, net::Request& out, std::string& error) {
    lua::Api& api = lua::GetApi();
    if (api.type(L, index) != lua::kTypeTable) {
        error = "table expected";
        return false;
    }
    if (!api.getfield(L, index, "Url") && !api.getfield(L, index, "url")) {
        api.pop(L, 1);
        error = "Url is required";
        return false;
    }
    const char* url = api.tostring ? api.tostring(L, -1) : nullptr;
    if (!url) {
        api.pop(L, 1);
        error = "Url must be a string";
        return false;
    }
    out.url = url;
    api.pop(L, 1);

    if (api.getfield(L, index, "Method")) {
        const char* method = api.tostring ? api.tostring(L, -1) : nullptr;
        if (method) out.method = method;
        api.pop(L, 1);
    }
    if (api.getfield(L, index, "Body")) {
        const char* body = api.tostring ? api.tostring(L, -1) : nullptr;
        if (body) out.body = body;
        api.pop(L, 1);
    }
    if (api.getfield(L, index, "Headers")) {
        if (api.type(L, -1) == lua::kTypeTable && api.next) {
            const int headersAbs = api.gettop(L);
            api.pushnil(L);                                      // [headers][nil]
            while (api.next(L, headersAbs)) {                    // [headers][key][value]
                const char* key = api.tostring ? api.tostring(L, -2) : nullptr;
                const char* value = api.tostring ? api.tostring(L, -1) : nullptr;
                if (key && value) out.headers.emplace_back(key, value);
                api.pop(L, 1);                                   // keep the key for lua_next
                if (out.headers.size() > 64) break;              // bounded
            }
        }
        api.pop(L, 1);
    }
    return true;
}

// Push a response table: { StatusCode, StatusMessage, Headers, Body, Success }
void PushResponse(lua_State* L, const net::Response& response) {
    lua::Api& api = lua::GetApi();
    api.createtable(L, 0, 5);

    PushNumber(L, response.status);
    api.setfield(L, -2, "StatusCode");
    PushString(L, response.statusMessage.c_str());
    api.setfield(L, -2, "StatusMessage");
    PushBool(L, response.ok);
    api.setfield(L, -2, "Success");
    PushString(L, response.error.c_str());
    api.setfield(L, -2, "ErrorMessage");

    api.createtable(L, 0, static_cast<int>(response.headers.size()));
    for (const auto& [key, value] : response.headers) {
        PushString(L, value.c_str());
        api.setfield(L, -2, key.c_str());
    }
    api.setfield(L, -2, "Headers");

    if (api.pushlstring) api.pushlstring(L, response.body.data(), response.body.size());
    else PushString(L, response.body.c_str());
    api.setfield(L, -2, "Body");
}

}  // namespace

// request(table) → response table. Called from a script thread (main thread);
// the HTTP work itself happens on a worker created by net::client's caller —
// here we do it inline on a std::thread and join, because the drain must not
// block. A script that calls request() will therefore yield the frame; the
// async variants exist for scripts that care.
int l_request(lua_State* L) {
    net::Request request{};
    std::string error;
    if (!ReadRequest(L, 1, request, error)) {
        Raise(L, ("request: " + error).c_str());
        return 0;
    }

    net::Response response{};
    std::thread worker([&] { response = net::Send(request); });
    worker.join();

    PushResponse(L, response);
    return 1;
}

int l_httpget(lua_State* L) {
    const char* url = ToString(L, 1);
    if (!url) { Raise(L, "httpget: url expected"); return 0; }
    net::Response response{};
    std::thread worker([&] { response = net::Get(url); });
    worker.join();
    PushResponse(L, response);
    return 1;
}

int l_httppost(lua_State* L) {
    const char* url = ToString(L, 1);
    const char* body = ToString(L, 2);
    if (!url) { Raise(L, "httppost: url expected"); return 0; }
    net::Response response{};
    std::thread worker([&] { response = net::Post(url, body ? body : ""); });
    worker.join();
    PushResponse(L, response);
    return 1;
}

// ---- async -------------------------------------------------------------------
// The callback is pinned by ref; a detached worker performs the request and then
// posts a CallInto job whose argument is an AsyncCtx. The trampoline runs on the
// main thread and frees the context — no path leaks it, even on scheduler
// shutdown (the job is discarded with the ring).

struct AsyncCtx {
    int callbackRef = -1;
    std::string body;
    long status = 0;
    bool ok = false;
    std::string error;
};

void InvokeAsyncCallback(void* raw) {
    AsyncCtx* ctx = static_cast<AsyncCtx*>(raw);
    if (!ctx) return;
    lua::Api& api = lua::GetApi();
    lua_State* L = sched::MainState();
    if (L) {
        __try {
            if (api.rawgeti(L, lua::kRegistryIndex, ctx->callbackRef)) {
                if (ctx->ok) {
                    if (api.pushlstring) api.pushlstring(L, ctx->body.data(), ctx->body.size());
                    else api.pushstring(L, ctx->body.c_str());
                    PushNumber(L, static_cast<double>(ctx->status));
                } else {
                    PushNil(L);
                    PushString(L, ctx->error.c_str());
                }
                api.pcall(L, ctx->ok ? 2 : 2, 0, 0);
                api.settop(L, 0);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            log::Error("net: async callback %d faulted", ctx->callbackRef);
        }
        if (api.unref) api.unref(L, ctx->callbackRef);
    }
    delete ctx;
}

int AsyncRequest(lua_State* L, bool get) {
    lua::Api& api = lua::GetApi();
    const char* url = ToString(L, get ? 1 : 1);
    const char* arg2 = ToString(L, 2);
    const int callbackIndex = get ? 2 : 3;

    if (!url) { Raise(L, "async request: url expected"); return 0; }
    if (api.type(L, callbackIndex) != lua::kTypeFunction) {
        Raise(L, "async request: callback function expected");
        return 0;
    }
    const int ref = api.ref(L, callbackIndex);
    if (ref < 0) { Raise(L, "async request: could not pin the callback"); return 0; }

    std::string urlCopy(url);
    std::string bodyCopy(arg2 ? arg2 : "");
    std::thread worker([urlCopy, bodyCopy, ref, get] {
        AsyncCtx* ctx = new AsyncCtx{};
        ctx->callbackRef = ref;
        const net::Response response = get ? net::Get(urlCopy) : net::Post(urlCopy, bodyCopy);
        ctx->ok = response.ok;
        ctx->body = response.body;
        ctx->status = static_cast<long>(response.status);
        ctx->error = response.error;

        sched::Job job{};
        job.kind = sched::JobKind::CallInto;
        job.data = reinterpret_cast<void*>(&InvokeAsyncCallback);
        job.arg = reinterpret_cast<uintptr_t>(ctx);
        if (sched::Enqueue(job) == 0) {
            // Queue full (the scheduler is stopping, or a script flooded it).
            // We deliberately do NOT unref the callback here: lua_unref is a VM
            // call and this is a worker thread — touching the VM off the main
            // thread is exactly what ADR-5 forbids. One leaked ref per dropped
            // job is bounded by the drop counter and is the safe failure.
            log::Warn("net: async job dropped (queue full) — callback ref %d leaked deliberately", ref);
            delete ctx;
        }
    });
    worker.detach();
    PushBool(L, true);
    return 1;
}

int l_httpgetasync(lua_State* L) { return AsyncRequest(L, true); }
int l_getasync(lua_State* L) { return AsyncRequest(L, true); }
int l_postasync(lua_State* L) { return AsyncRequest(L, false); }

}  // namespace phetamine::api
