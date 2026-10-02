// native_api/misc.cpp — clipboard, fps cap, teleport queue, hashing, crypt,
// loadstring, PHETAMINE.capability.
//
// `queue_on_teleport` is trivial now that the DLL survives teleports: the source
// is appended to a DLL-resident list and the watchdog replays it after each
// session rebind. No in-game persistence, no marker folder.
//
// Hashing and AES go through CNG (bcrypt) rather than a bundled crypto library:
// it is the platform's own implementation, it is already loaded in every
// process, and it keeps this file honest about what it is doing.
#include "native_api/api.h"
#include "lua/state.h"
#include "lua/env.h"
#include "executor/executor.h"
#include "core/core.h"
#include "memory/offsets.h"
#include "common/log.h"
#include "common/seh.h"

#include <windows.h>
#include <bcrypt.h>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace phetamine::api {
namespace {

void Raise(lua_State* L, const char* message) {
    lua::Api& api = lua::GetApi();
    api.pushstring(L, message);
    if (api.error) api.error(L);
}

std::string Base64Encode(const std::string& input) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((input.size() + 2) / 3) * 4);
    size_t i = 0;
    while (i + 2 < input.size()) {
        const uint32_t triple = (static_cast<uint8_t>(input[i]) << 16) |
                                (static_cast<uint8_t>(input[i + 1]) << 8) |
                                static_cast<uint8_t>(input[i + 2]);
        out += kAlphabet[(triple >> 18) & 0x3F];
        out += kAlphabet[(triple >> 12) & 0x3F];
        out += kAlphabet[(triple >> 6) & 0x3F];
        out += kAlphabet[triple & 0x3F];
        i += 3;
    }
    if (i < input.size()) {
        const size_t remaining = input.size() - i;
        uint32_t triple = static_cast<uint8_t>(input[i]) << 16;
        if (remaining == 2) triple |= static_cast<uint8_t>(input[i + 1]) << 8;
        out += kAlphabet[(triple >> 18) & 0x3F];
        out += kAlphabet[(triple >> 12) & 0x3F];
        out += remaining == 2 ? kAlphabet[(triple >> 6) & 0x3F] : '=';
        out += '=';
    }
    return out;
}

bool Base64Decode(const std::string& input, std::string& out) {
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    out.clear();
    int buffer = 0, bits = 0;
    for (char c : input) {
        if (c == '=' || c == '\n' || c == '\r') continue;
        const int v = value(c);
        if (v < 0) return false;
        buffer = (buffer << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((buffer >> bits) & 0xFF);
        }
    }
    return true;
}

// CNG symmetric encryption: AES-CBC with PKCS#7 padding (the shape every script
// expects from crypt.encrypt).
bool AesCrypt(bool encrypt, const std::string& data, const std::string& key,
              const std::string& iv, std::string& out, std::string& error) {
    if (key.empty() || (key.size() != 16 && key.size() != 24 && key.size() != 32)) {
        error = "key must be 16, 24 or 32 bytes";
        return false;
    }
    if (iv.size() != 16) {
        error = "iv must be 16 bytes";
        return false;
    }
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0) < 0) {
        error = "BCryptOpenAlgorithmProvider failed";
        return false;
    }
    // Chaining mode must be set before the key object is created.
    (void)BCryptSetProperty(alg, BCRYPT_CHAINING_MODE,
                            reinterpret_cast<PUCHAR>(const_cast<char*>(BCRYPT_CHAIN_MODE_CBC)),
                            sizeof(BCRYPT_CHAIN_MODE_CBC), 0);

    DWORD keyObjectSize = 0, blockSize = 0, result = 0;
    BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&keyObjectSize), sizeof(keyObjectSize), &result, 0);
    BCryptGetProperty(alg, BCRYPT_BLOCK_LENGTH, reinterpret_cast<PUCHAR>(&blockSize), sizeof(blockSize), &result, 0);

    std::vector<uint8_t> keyObject(keyObjectSize);
    BCRYPT_KEY_HANDLE keyHandle = nullptr;
    if (BCryptGenerateSymmetricKey(alg, &keyHandle, keyObject.data(), keyObjectSize,
                                   reinterpret_cast<PUCHAR>(const_cast<char*>(key.data())),
                                   static_cast<ULONG>(key.size()), 0) < 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        error = "BCryptGenerateSymmetricKey failed";
        return false;
    }

    std::vector<uint8_t> ivCopy(iv.begin(), iv.end());
    std::vector<uint8_t> buffer(data.size() + blockSize);
    ULONG written = 0;
    const PUCHAR input = reinterpret_cast<PUCHAR>(const_cast<char*>(data.data()));
    const NTSTATUS status = encrypt
        ? BCryptEncrypt(keyHandle, input, static_cast<ULONG>(data.size()), ivCopy.data(), nullptr, 0,
                        buffer.data(), static_cast<ULONG>(buffer.size()), &written, 0)
        : BCryptDecrypt(keyHandle, input, static_cast<ULONG>(data.size()), ivCopy.data(), nullptr, 0,
                        buffer.data(), static_cast<ULONG>(buffer.size()), &written, 0);

    BCryptDestroyKey(keyHandle);
    BCryptCloseAlgorithmProvider(alg, 0);
    if (status < 0) {
        error = encrypt ? "BCryptEncrypt failed" : "BCryptDecrypt failed";
        return false;
    }
    out.assign(reinterpret_cast<char*>(buffer.data()), written);
    return true;
}

bool HashBytes(const std::string& algorithm, const std::string& data, std::string& out) {
    LPCWSTR algId = nullptr;
    if (algorithm == "sha256") algId = BCRYPT_SHA256_ALGORITHM;
    else if (algorithm == "sha1") algId = BCRYPT_SHA1_ALGORITHM;
    else if (algorithm == "md5") algId = BCRYPT_MD5_ALGORITHM;
    else return false;

    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, algId, nullptr, 0) < 0) return false;

    DWORD objectSize = 0, hashSize = 0, result = 0;
    BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &result, 0);
    BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashSize), sizeof(hashSize), &result, 0);

    std::vector<uint8_t> object(objectSize), hash(hashSize);
    BCRYPT_HASH_HANDLE handle = nullptr;
    bool ok = BCryptCreateHash(alg, &handle, object.data(), objectSize, nullptr, 0, 0) >= 0;
    if (ok) ok = BCryptHashData(handle, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),
                                static_cast<ULONG>(data.size()), 0) >= 0;
    if (ok) ok = BCryptFinishHash(handle, hash.data(), hashSize, 0) >= 0;
    if (handle) BCryptDestroyHash(handle);
    BCryptCloseAlgorithmProvider(alg, 0);
    if (!ok) return false;

    static constexpr char kHex[] = "0123456789abcdef";
    out.clear();
    for (uint8_t byte : hash) {
        out += kHex[byte >> 4];
        out += kHex[byte & 0xF];
    }
    return true;
}

}  // namespace

int l_identifyexecutor(lua_State* L) {
    PushString(L, "PHETAMINE");
    PushString(L, "1.0.0-native");
    return 2;
}

int l_getexecutorname(lua_State* L) {
    PushString(L, "PHETAMINE");
    return 1;
}

int l_setclipboard(lua_State* L) {
    const char* text = ToString(L, 1);
    if (!text) { Raise(L, "setclipboard: string expected"); return 0; }
    const size_t length = std::strlen(text);
    if (!OpenClipboard(nullptr)) { Raise(L, "setclipboard: OpenClipboard failed"); return 0; }
    EmptyClipboard();
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, length + 1);
    if (memory) {
        if (void* target = GlobalLock(memory)) {
            std::memcpy(target, text, length + 1);
            GlobalUnlock(memory);
            SetClipboardData(CF_TEXT, memory);
        }
    }
    CloseClipboard();
    PushBool(L, true);
    return 1;
}

int l_messagebox(lua_State* L) {
    const char* text = ToString(L, 1);
    const char* title = ToString(L, 2);
    if (!text) { Raise(L, "messagebox: string expected"); return 0; }
    // Modal on a worker: blocking the drain on a user click would freeze the
    // client, which is exactly the kind of "feature causes the crash" the design
    // avoids.
    std::string message(text), caption(title ? title : "PHETAMINE");
    std::thread([message, caption] {
        MessageBoxA(nullptr, message.c_str(), caption.c_str(), MB_OK | MB_ICONINFORMATION);
    }).detach();
    PushBool(L, true);
    return 1;
}

int l_getfpscap(lua_State* L) {
    const uintptr_t scheduler = off::Get("task_scheduler.pointer");
    const uintptr_t offset = off::Get("task_scheduler.max_fps");
    if (!scheduler || !offset) { PushNumber(L, 0); return 1; }
    uintptr_t ts = 0;
    if (!seh::TryReadPtr(scheduler, ts) || !ts) { PushNumber(L, 0); return 1; }
    int32_t cap = 0;
    if (!seh::TryRead<int32_t>(ts + offset, cap)) { PushNumber(L, 0); return 1; }
    PushNumber(L, cap);
    return 1;
}

int l_setfpscap(lua_State* L) {
    lua::Api& api = lua::GetApi();
    const uintptr_t scheduler = off::Get("task_scheduler.pointer");
    const uintptr_t offset = off::Get("task_scheduler.max_fps");
    if (!scheduler || !offset) {
        Raise(L, "setfpscap: TaskScheduler::MaxFPS is not in the table for this build");
        return 0;
    }
    uintptr_t ts = 0;
    if (!seh::TryReadPtr(scheduler, ts) || !ts) {
        Raise(L, "setfpscap: the task scheduler is not reachable yet");
        return 0;
    }
    const int32_t cap = static_cast<int32_t>(api.tointeger(L, 1, nullptr));
    if (!seh::TryWrite<int32_t>(ts + offset, cap)) {
        off::MarkProbed("task_scheduler.max_fps", false);
        Raise(L, "setfpscap: the write failed (probe says this offset moved)");
        return 0;
    }
    off::MarkProbed("task_scheduler.max_fps", true);
    PushBool(L, true);
    return 1;
}

int l_queue_on_teleport(lua_State* L) {
    lua::Api& api = lua::GetApi();
    const char* source = ToString(L, 1);
    if (!source) { Raise(L, "queue_on_teleport: source string expected"); return 0; }
    size_t length = 0;
    if (api.tolstring) api.tolstring(L, 1, &length);
    core::QueueOnTeleport(std::string(source, length));
    log::Info("queue_on_teleport: queued %zu byte(s); the watchdog replays them after the next "
              "session rebind", length);
    PushBool(L, true);
    return 1;
}

// securecall(fn, ...) — like pcall, but it does not hide the fact that the call
// came from an executor thread: checkcaller() inside the callback still answers
// true, because the wrapper runs on the same (our) thread.
int l_securecall(lua_State* L) {
    lua::Api& api = lua::GetApi();
    if (api.type(L, 1) != lua::kTypeFunction) {
        Raise(L, "securecall: function expected");
        return 0;
    }
    const int nargs = api.gettop(L) - 1;
    const int status = api.pcall(L, nargs, lua::kMultiRet, 0);
    api.pushboolean(L, status == lua::kOk ? 1 : 0);
    api.insert(L, 1);                 // results..., ok
    return api.gettop(L);
}

int l_isrbxactive(lua_State* L) {
    HWND foreground = GetForegroundWindow();
    DWORD pid = 0;
    if (foreground) GetWindowThreadProcessId(foreground, &pid);
    PushBool(L, pid == GetCurrentProcessId());
    return 1;
}

int l_hash(lua_State* L) {
    const char* data = ToString(L, 1);
    const char* algorithm = ToString(L, 2);
    if (!data || !algorithm) { Raise(L, "hash: data and algorithm expected"); return 0; }
    std::string digest;
    if (!HashBytes(algorithm, data, digest)) {
        Raise(L, "hash: unknown algorithm (sha256, sha1, md5)");
        return 0;
    }
    PushString(L, digest.c_str());
    return 1;
}

int l_crypt_b64_encode(lua_State* L) {
    const char* data = ToString(L, 1);
    if (!data) { Raise(L, "crypt.base64encode: string expected"); return 0; }
    const std::string encoded = Base64Encode(data);
    PushString(L, encoded.c_str());
    return 1;
}

int l_crypt_b64_decode(lua_State* L) {
    const char* data = ToString(L, 1);
    if (!data) { Raise(L, "crypt.base64decode: string expected"); return 0; }
    std::string decoded;
    if (!Base64Decode(data, decoded)) { Raise(L, "crypt.base64decode: invalid input"); return 0; }
    lua::Api& api = lua::GetApi();
    if (api.pushlstring) api.pushlstring(L, decoded.data(), decoded.size());
    else PushString(L, decoded.c_str());
    return 1;
}

int l_crypt_encrypt(lua_State* L) {
    const char* data = ToString(L, 1);
    const char* key = ToString(L, 2);
    const char* iv = ToString(L, 3);
    if (!data || !key || !iv) { Raise(L, "crypt.encrypt: data, key and iv expected"); return 0; }
    std::string out, error;
    if (!AesCrypt(true, data, key, iv, out, error)) { Raise(L, ("crypt.encrypt: " + error).c_str()); return 0; }
    lua::Api& api = lua::GetApi();
    if (api.pushlstring) api.pushlstring(L, out.data(), out.size());
    else PushString(L, out.c_str());
    return 1;
}

int l_crypt_decrypt(lua_State* L) {
    const char* data = ToString(L, 1);
    const char* key = ToString(L, 2);
    const char* iv = ToString(L, 3);
    if (!data || !key || !iv) { Raise(L, "crypt.decrypt: data, key and iv expected"); return 0; }
    std::string out, error;
    if (!AesCrypt(false, data, key, iv, out, error)) { Raise(L, ("crypt.decrypt: " + error).c_str()); return 0; }
    lua::Api& api = lua::GetApi();
    if (api.pushlstring) api.pushlstring(L, out.data(), out.size());
    else PushString(L, out.c_str());
    return 1;
}

int l_crypt_generatekey(lua_State* L) {
    lua::Api& api = lua::GetApi();
    int length = static_cast<int>(api.tointeger(L, 1, nullptr));
    if (length != 16 && length != 24 && length != 32) length = 32;
    std::string key(static_cast<size_t>(length), '\0');
    if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(key.data()), length, BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
        Raise(L, "crypt.generatekey: BCryptGenRandom failed");
        return 0;
    }
    if (api.pushlstring) api.pushlstring(L, key.data(), key.size());
    else PushString(L, key.c_str());
    return 1;
}

int l_crypt_generateiv(lua_State* L) {
    lua::Api& api = lua::GetApi();
    std::string iv(16, '\0');
    if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(iv.data()), 16, BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
        Raise(L, "crypt.generateiv: BCryptGenRandom failed");
        return 0;
    }
    if (api.pushlstring) api.pushlstring(L, iv.data(), iv.size());
    else PushString(L, iv.c_str());
    return 1;
}

int l_loadstring_native(lua_State* L) {
    lua::Api& api = lua::GetApi();
    const char* source = ToString(L, 1);
    if (!source) { Raise(L, "loadstring: string expected"); return 0; }
    size_t length = 0;
    if (api.tolstring) api.tolstring(L, 1, &length);
    std::string error;
    if (!exec::LoadStringInto(L, source, length, error)) {
        PushNil(L);
        PushString(L, error.c_str());
        return 2;
    }
    return 1;   // the function, pushed by LoadStringInto
}

int l_capability(lua_State* L) {
    const char* name = ToString(L, 1);
    if (!name) { Raise(L, "PHETAMINE.capability: name expected"); return 0; }
    const Cap cap = CapOf(name);
    if (cap == Cap::Count) {
        PushNil(L);      // unknown names answer nil, not a misleading false
        return 1;
    }
    PushBool(L, Has(cap));
    return 1;
}

}  // namespace phetamine::api
