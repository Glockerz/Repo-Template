// memory/instance_walker.h — the (now small) instance layer.
//
// Reached through the offset table, never through constants. The dump drives
// these shapes (docs/OFFSETS.md §3):
//
//   name:      instance + NameContainer(0x70) → + Name(0x08) → SSO object
//   children:  instance + ChildrenStart(0x78) → container → [start..end), stride 16
//   parent:    instance + Parent(0x68)
//   class:     instance + ClassDescriptor(0x18) → + ClassName(0x08) → SSO object
//
// Every read is SEH-guarded and every list is bounded: a mis-resolved offset
// yields "no children", not a walk into unmapped memory.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace phetamine::mem {

inline constexpr int  kMaxChildScan = 4096;   // a parent with more children than this is not a real parent
inline constexpr int  kMaxDepth     = 12;
inline constexpr int  kMaxInstances = 200000;

struct Instance {
    uintptr_t address = 0;
    explicit operator bool() const { return address != 0; }
};

bool        IsValidInstance(uintptr_t inst);                    // SEH probe + class-descriptor sanity
std::string GetClassName(uintptr_t inst);
std::string GetName(uintptr_t inst);
bool        IsA(uintptr_t inst, const char* className);        // class-name compare (no inheritance walk)
uintptr_t   GetParent(uintptr_t inst);
std::vector<uintptr_t> GetChildren(uintptr_t inst);
uintptr_t   FindFirstChild(uintptr_t parent, const char* name);
uintptr_t   FindFirstByClass(uintptr_t root, const char* className, int maxDepth = kMaxDepth);
std::vector<uintptr_t> GetDescendants(uintptr_t root, int maxDepth = kMaxDepth);

// ---- engine anchors ----------------------------------------------------------
uintptr_t GetDataModel();                       // FakeDataModel::Pointer → RealDataModel, else job walk
uintptr_t AcquireScriptContext(uintptr_t dataModel);
uintptr_t GetLocalPlayer(uintptr_t dataModel);
uintptr_t GetGameId(uintptr_t dataModel);
uintptr_t GetPlaceId(uintptr_t dataModel);      // via the ScriptContext's DataModel, offset-probed
std::string GetJobId(uintptr_t dataModel);

// ---- hidden container (gethui) ----------------------------------------------
// Parented to PlayerGui, NOT CoreGui: the identity-2 input restriction crash is
// engine behaviour around CoreGui and has nothing to do with being internal.
// Cached per session; cleared on rebind (the old container dies with the place).
uintptr_t GetHiddenContainer();
void      ClearHiddenContainer();
uintptr_t EnsureHiddenContainer();

}  // namespace phetamine::mem
