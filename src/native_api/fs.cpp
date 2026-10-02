// native_api/fs.cpp — workspace-sandboxed filesystem functions.
//
// Every path is resolved against ONE root (set by core from the loader's param
// block). `..` segments are rejected after normalisation, not before, so
// "a/../b" works while "../../windows/system32" cannot escape. A script can only
// ever touch the workspace it was pointed at.
#include "native_api/api.h"
#include "common/log.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace phetamine::api {
namespace {

namespace fsys = std::filesystem;

std::wstring g_root;

void Raise(lua_State* L, const std::string& message) {
    lua::Api& api = lua::GetApi();
    api.pushstring(L, message.c_str());
    if (api.error) api.error(L);
}

// Resolves a script-supplied relative path inside the workspace root. Returns an
// empty path and a reason when the path is missing, absolute, or escapes.
bool Resolve(lua_State* L, int index, fsys::path& out, std::string& reason) {
    const char* raw = ToString(L, index);
    if (!raw || !*raw) {
        reason = "path (string) expected";
        return false;
    }
    if (g_root.empty()) {
        reason = "workspace root is not set (loader did not pass one)";
        return false;
    }
    fsys::path candidate = fsys::path(raw);
    if (candidate.is_absolute()) {
        reason = "absolute paths are rejected";
        return false;
    }
    fsys::path normalised;
    for (const auto& part : candidate) {
        if (part == "..") {
            reason = "path escapes the workspace";
            return false;
        }
        if (part == "." || part.empty()) continue;
        normalised /= part;
    }
    out = fsys::path(g_root) / normalised;
    return true;
}

}  // namespace

void SetWorkspaceRoot(const wchar_t* root) { g_root = root ? root : L""; }
const wchar_t* WorkspaceRoot() { return g_root.c_str(); }

int l_readfile(lua_State* L) {
    fsys::path path;
    std::string reason;
    if (!Resolve(L, 1, path, reason)) { Raise(L, "readfile: " + reason); return 0; }
    std::ifstream in(path, std::ios::binary);
    if (!in) { Raise(L, "readfile: cannot open " + path.filename().string()); return 0; }
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    lua::Api& api = lua::GetApi();
    if (api.pushlstring) api.pushlstring(L, content.data(), content.size());
    else PushString(L, content.c_str());
    return 1;
}

int l_writefile(lua_State* L) {
    fsys::path path;
    std::string reason;
    if (!Resolve(L, 1, path, reason)) { Raise(L, "writefile: " + reason); return 0; }
    const char* data = ToString(L, 2);
    if (!data) { Raise(L, "writefile: string expected"); return 0; }
    std::error_code ec;
    fsys::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) { Raise(L, "writefile: cannot open " + path.filename().string()); return 0; }
    size_t length = 0;
    __try {
        // tostring() may return nullptr for non-strings; the check above covers it
        length = strlen(data);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        length = 0;
    }
    out.write(data, static_cast<std::streamsize>(length));
    PushBool(L, true);
    return 1;
}

int l_appendfile(lua_State* L) {
    fsys::path path;
    std::string reason;
    if (!Resolve(L, 1, path, reason)) { Raise(L, "appendfile: " + reason); return 0; }
    const char* data = ToString(L, 2);
    if (!data) { Raise(L, "appendfile: string expected"); return 0; }
    std::ofstream out(path, std::ios::binary | std::ios::app);
    if (!out) { Raise(L, "appendfile: cannot open " + path.filename().string()); return 0; }
    out << data;
    PushBool(L, true);
    return 1;
}

int l_listfiles(lua_State* L) {
    fsys::path path;
    std::string reason;
    if (!Resolve(L, 1, path, reason)) { Raise(L, "listfiles: " + reason); return 0; }
    lua::Api& api = lua::GetApi();
    api.createtable(L, 0, 8);
    int index = 1;
    std::error_code ec;
    if (fsys::exists(path, ec) && fsys::is_directory(path, ec)) {
        for (const auto& entry : fsys::directory_iterator(path, ec)) {
            const std::string name = entry.path().string();
            PushString(L, name.c_str());
            api.rawseti(L, -2, index++);
            if (index > 4096) break;      // bounded: a pathological folder cannot hang a script
        }
    }
    return 1;
}

int l_isfile(lua_State* L) {
    fsys::path path;
    std::string reason;
    if (!Resolve(L, 1, path, reason)) { PushBool(L, false); return 1; }
    std::error_code ec;
    PushBool(L, fsys::is_regular_file(path, ec));
    return 1;
}

int l_isfolder(lua_State* L) {
    fsys::path path;
    std::string reason;
    if (!Resolve(L, 1, path, reason)) { PushBool(L, false); return 1; }
    std::error_code ec;
    PushBool(L, fsys::is_directory(path, ec));
    return 1;
}

int l_makefolder(lua_State* L) {
    fsys::path path;
    std::string reason;
    if (!Resolve(L, 1, path, reason)) { Raise(L, "makefolder: " + reason); return 0; }
    std::error_code ec;
    fsys::create_directories(path, ec);
    PushBool(L, !ec);
    return 1;
}

int l_delfile(lua_State* L) {
    fsys::path path;
    std::string reason;
    if (!Resolve(L, 1, path, reason)) { Raise(L, "delfile: " + reason); return 0; }
    std::error_code ec;
    PushBool(L, fsys::remove(path, ec));
    return 1;
}

int l_delfolder(lua_State* L) {
    fsys::path path;
    std::string reason;
    if (!Resolve(L, 1, path, reason)) { Raise(L, "delfolder: " + reason); return 0; }
    std::error_code ec;
    fsys::remove_all(path, ec);
    PushBool(L, !ec);
    return 1;
}

}  // namespace phetamine::api
