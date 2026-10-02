// executor/executor.h — compile → load → run.
//
// Worker side (IPC thread): compile the source with the CLIENT's luau_compile
// and hand the buffer to the scheduler.
// Main thread (drain): create a fresh thread, load the bytecode with genv as its
// env, apply identity 8, resume. Errors are captured with lua_tostring and sent
// straight back over IPC — there is no Lua worker to survive or not survive.
//
// Ownership rule: a compile buffer is allocated by the client's allocator and
// MUST be released with the client's free (resolved as `crt.free`). Our DLL is
// /MT, so our own free() is a different heap — see docs/PHETAMINE.md §5.
#pragma once

#include <cstdint>
#include <string>

struct lua_State;

namespace phetamine::exec {

struct Bytecode {
    char*  data = nullptr;      // client-allocated
    size_t size = 0;
    bool   owned = false;       // true until handed to a scheduler job

    Bytecode() = default;
    Bytecode(const Bytecode&) = delete;
    Bytecode& operator=(const Bytecode&) = delete;
    Bytecode(Bytecode&& other) noexcept;
    Bytecode& operator=(Bytecode&& other) noexcept;
    ~Bytecode();

    void Reset();               // frees through the client's allocator
};

// Records the session environment the drain will load scripts with. Set by core
// once per bind (init + every teleport rebind).
namespace lua::env { struct Environment; }
void SetEnvironment(const lua::env::Environment& env);

// Compiles `source`. On failure returns false and fills `error`.
// NOTE: the fork's C entry point does not report *why* compilation failed, so a
// syntax error surfaces as a generic message here; a diagnostic path through the
// bundled parser is planned (docs/UNC_COVERAGE.md, `loadstring`).
bool Compile(const char* source, size_t length, Bytecode& out, std::string& error);

// Worker side: compile + enqueue. Returns the job id (0 on failure).
uint64_t Run(const std::string& source, std::string& error);

// Main-thread helper used by the `loadstring` C closure: compiles and pushes a
// function on the caller's stack. Returns false and pushes the error string.
bool LoadStringInto(lua_State* L, const char* source, size_t length, std::string& error);

// Stops every script we own: lua_resetthread when available, retire the ref
// either way (an abandoned thread is far safer than a corrupted one).
void StopAll();

// Implementation detail shared with the scheduler: the thread currently being
// drained, so `checkcaller()` and `getcallingscript()` can answer honestly.
lua_State* CurrentScriptThread();
void SetCurrentScriptThread(lua_State* T);

}  // namespace phetamine::exec
