#include "memory/instance_walker.h"
#include "memory/offsets.h"
#include "common/log.h"
#include "common/strings.h"
#include "common/seh.h"

#include <cstring>
#include <unordered_set>

namespace phetamine::mem {
namespace {

uintptr_t g_hiddenContainer = 0;

// Offsets are read once and cached; a 0 result means "not in the table", which
// every helper below treats as "this feature is off", never as "offset zero".
struct Layout {
    uintptr_t parent = 0;
    uintptr_t classDescriptor = 0;
    uintptr_t className = 0;
    uintptr_t nameContainer = 0;
    uintptr_t nameInContainer = 0;
    uintptr_t childrenStart = 0;
    uintptr_t childrenEnd = 0;
    uintptr_t stringLength = 0;
    uintptr_t selfRef = 0;

    bool load() {
        parent          = off::Get("instance.parent");
        classDescriptor = off::Get("instance.class_descriptor");
        className       = off::Get("instance.class_name");
        nameContainer   = off::Get("instance.name_container");
        nameInContainer = off::Get("instance.name_in_container");
        childrenStart   = off::Get("instance.children_start");
        childrenEnd     = off::Get("instance.children_end");
        stringLength    = off::Get("misc.string_length");
        selfRef         = off::Get("instance.this");
        return classDescriptor && className && stringLength;
    }
};

const Layout& L() {
    static Layout layout = [] { Layout l; l.load(); return l; }();
    return layout;
}

struct ChildWalk {
    uintptr_t container = 0;
    uintptr_t start = 0;
    uintptr_t end = 0;
    bool ok = false;
};

ChildWalk OpenChildList(uintptr_t inst) {
    ChildWalk w{};
    const Layout& l = L();
    if (!l.childrenStart || !l.childrenEnd) return w;
    if (!seh::TryReadPtr(inst + l.childrenStart, w.container) || w.container < 0x10000) return w;
    if (!seh::TryReadPtr(w.container, w.start)) return w;
    if (!seh::TryReadPtr(w.container + l.childrenEnd, w.end)) return w;
    if (w.start < 0x10000 || w.end < w.start) return w;
    w.ok = true;
    return w;
}

}  // namespace

bool IsValidInstance(uintptr_t inst) {
    const Layout& l = L();
    if (inst < 0x10000 || !l.classDescriptor) return false;

    uintptr_t descriptor = 0;
    if (!seh::TryReadPtr(inst + l.classDescriptor, descriptor)) return false;
    if (descriptor < 0x10000) return false;

    // The descriptor must yield a plausible, non-empty class name. This is the
    // strongest cheap check we have: it fails for freed objects (descriptor
    // nulled) and for raw pointers that merely point at readable memory.
    const std::string cls = str::ReadSso(descriptor + l.className, l.stringLength);
    if (cls.empty() || cls.size() > 128) return false;

    // Self-reference check, when the dump provides the field: instance+This
    // points back at the instance. It is the cheapest "this is really an
    // Instance" test and costs one read.
    if (l.selfRef) {
        uintptr_t self = 0;
        if (seh::TryReadPtr(inst + l.selfRef, self) && self != inst) return false;
    }
    return true;
}

std::string GetClassName(uintptr_t inst) {
    const Layout& l = L();
    if (!IsValidInstance(inst)) return {};
    uintptr_t descriptor = 0;
    if (!seh::TryReadPtr(inst + l.classDescriptor, descriptor)) return {};
    return str::ReadSso(descriptor + l.className, l.stringLength);
}

std::string GetName(uintptr_t inst) {
    const Layout& l = L();
    if (inst < 0x10000 || !l.nameContainer) return {};
    uintptr_t container = 0;
    if (!seh::TryReadPtr(inst + l.nameContainer, container) || container < 0x10000) return {};
    return str::ReadSso(container + l.nameInContainer, l.stringLength);
}

bool IsA(uintptr_t inst, const char* className) {
    if (!className) return false;
    return str::IEquals(GetClassName(inst), className);
}

uintptr_t GetParent(uintptr_t inst) {
    const Layout& l = L();
    if (!l.parent) return 0;
    uintptr_t parent = 0;
    if (!seh::TryReadPtr(inst + l.parent, parent)) return 0;
    return parent < 0x10000 ? 0 : parent;
}

std::vector<uintptr_t> GetChildren(uintptr_t inst) {
    std::vector<uintptr_t> out;
    const ChildWalk w = OpenChildList(inst);
    if (!w.ok) return out;
    out.reserve(16);
    for (uintptr_t node = w.start; node != w.end; node += 16) {
        if (out.size() >= static_cast<size_t>(kMaxChildScan)) {
            log::Warn("walker: child list exceeded %d entries — stopping (bad offset?)", kMaxChildScan);
            break;
        }
        uintptr_t child = 0;
        if (!seh::TryReadPtr(node, child)) break;
        if (child < 0x10000) continue;
        out.push_back(child);
    }
    return out;
}

uintptr_t FindFirstChild(uintptr_t parent, const char* name) {
    if (!parent || !name) return 0;
    for (uintptr_t child : GetChildren(parent)) {
        if (str::IEquals(GetName(child), name)) return child;
    }
    return 0;
}

std::vector<uintptr_t> GetDescendants(uintptr_t root, int maxDepth) {
    std::vector<uintptr_t> out;
    struct Frame { uintptr_t inst; int depth; };
    std::vector<Frame> stack{ { root, 0 } };
    while (!stack.empty()) {
        const Frame f = stack.back();
        stack.pop_back();
        if (f.depth >= maxDepth) continue;
        for (uintptr_t child : GetChildren(f.inst)) {
            if (out.size() >= static_cast<size_t>(kMaxInstances)) {
                log::Warn("walker: descendant cap (%d) reached — stopping", kMaxInstances);
                return out;
            }
            out.push_back(child);
            stack.push_back({ child, f.depth + 1 });
        }
    }
    return out;
}

uintptr_t FindFirstByClass(uintptr_t root, const char* className, int maxDepth) {
    for (uintptr_t inst : GetDescendants(root, maxDepth)) {
        if (IsA(inst, className)) return inst;
    }
    return 0;
}

// ---- engine anchors ----------------------------------------------------------

uintptr_t GetDataModel() {
    // Primary: the FakeDataModel global → real DataModel.
    const uintptr_t fakeGlobal = off::Get("fake_data_model.pointer");
    const uintptr_t realOffset = off::Get("fake_data_model.real");
    if (fakeGlobal && realOffset) {
        uintptr_t fake = 0;
        if (seh::TryReadPtr(fakeGlobal, fake) && fake > 0x10000) {
            uintptr_t dm = 0;
            if (seh::TryReadPtr(fake + realOffset, dm) && dm > 0x10000 && IsA(dm, "DataModel")) {
                off::MarkProbed("fake_data_model.pointer", true);
                off::MarkProbed("fake_data_model.real", true);
                return dm;
            }
        }
        off::MarkProbed("fake_data_model.pointer", false);
    }

    // Fallback: walk TaskScheduler jobs looking for the RenderJob (it carries
    // the real DataModel). Both fields come from the table; if either is absent
    // this path is simply unavailable.
    const uintptr_t tsPtr = off::Get("task_scheduler.pointer");
    const uintptr_t jobStart = off::Get("task_scheduler.job_start");
    const uintptr_t jobEnd = off::Get("task_scheduler.job_end");
    const uintptr_t jobName = off::Get("task_scheduler.job_name");
    const uintptr_t rjFake = off::Get("render_job.fake_data_model");
    const uintptr_t rjReal = off::Get("render_job.real_data_model");
    if (!tsPtr || !jobStart || !jobEnd || !jobName || !rjFake || !rjReal) return 0;

    uintptr_t ts = 0;
    if (!seh::TryReadPtr(tsPtr, ts) || ts < 0x10000) return 0;
    uintptr_t begin = 0, end = 0;
    if (!seh::TryReadPtr(ts + jobStart, begin) || !seh::TryReadPtr(ts + jobEnd, end)) return 0;
    if (begin < 0x10000 || end <= begin) return 0;
    if ((end - begin) > (2u << 20)) return 0;             // sanity: job array is small

    for (uintptr_t job = begin; job + 8 <= end; job += 8) {
        uintptr_t entry = 0;
        if (!seh::TryReadPtr(job, entry) || entry < 0x10000) continue;
        // job name is an SSO string inside the job object
        const std::string name = str::ReadSso(entry + jobName, off::Get("misc.string_length"));
        if (!str::IEquals(name, "RenderJob")) continue;
        uintptr_t fake = 0;
        if (!seh::TryReadPtr(entry + rjFake, fake) || fake < 0x10000) break;
        uintptr_t dm = 0;
        if (!seh::TryReadPtr(fake + rjReal, dm) || dm < 0x10000) break;
        if (!IsA(dm, "DataModel")) break;
        log::Warn("walker: resolved DataModel via the TaskScheduler job walk (FakeDataModel global failed)");
        return dm;
    }
    return 0;
}

uintptr_t AcquireScriptContext(uintptr_t dataModel) {
    const uintptr_t offset = off::Get("data_model.script_context");
    if (!offset || !dataModel) return 0;
    uintptr_t sc = 0;
    if (!seh::TryReadPtr(dataModel + offset, sc) || sc < 0x10000) {
        off::MarkProbed("data_model.script_context", false);
        return 0;
    }
    if (!IsA(sc, "ScriptContext")) {
        log::Error("walker: DataModel+0x%llX is not a ScriptContext (class '%s')",
                   static_cast<unsigned long long>(offset), GetClassName(sc).c_str());
        off::MarkProbed("data_model.script_context", false);
        return 0;
    }
    off::MarkProbed("data_model.script_context", true);
    return sc;
}

uintptr_t GetLocalPlayer(uintptr_t dataModel) {
    const uintptr_t offset = off::Get("player.local_player");
    if (!offset || !dataModel) return 0;
    const uintptr_t players = FindFirstChild(dataModel, "Players");
    if (!players) return 0;
    uintptr_t lp = 0;
    if (!seh::TryReadPtr(players + offset, lp)) return 0;
    return lp < 0x10000 ? 0 : lp;
}

uintptr_t GetGameId(uintptr_t dataModel) {
    const uintptr_t offset = off::Get("data_model.game_id");
    if (!offset || !dataModel) return 0;
    uintptr_t id = 0;
    seh::TryReadPtr(dataModel + offset, id);
    return id;
}

uintptr_t GetPlaceId(uintptr_t dataModel) {
    const uintptr_t offset = off::Get("data_model.place_id");
    if (!offset || !dataModel) return 0;
    uintptr_t id = 0;
    seh::TryReadPtr(dataModel + offset, id);
    return id;
}

std::string GetJobId(uintptr_t dataModel) {
    const uintptr_t offset = off::Get("data_model.job_id");
    if (!offset || !dataModel) return {};
    return str::ReadSso(dataModel + offset, off::Get("misc.string_length"));
}

// ---- hidden container --------------------------------------------------------

uintptr_t GetHiddenContainer() { return g_hiddenContainer; }
void ClearHiddenContainer() { g_hiddenContainer = 0; }

uintptr_t EnsureHiddenContainer() {
    if (g_hiddenContainer && IsValidInstance(g_hiddenContainer)) return g_hiddenContainer;
    g_hiddenContainer = 0;

    const uintptr_t dm = GetDataModel();
    if (!dm) return 0;
    const uintptr_t players = FindFirstChild(dm, "Players");
    const uintptr_t localPlayer = GetLocalPlayer(dm);
    const uintptr_t playerGui = localPlayer ? FindFirstChild(localPlayer, "PlayerGui") : 0;
    (void)players;

    // The container is created by native_api/instances.cpp through the VM
    // (Instance.new + Parent), because creating an Instance natively would need
    // a resolved instance factory that the public dump does not publish. Here we
    // only *find* what that path created.
    if (playerGui) g_hiddenContainer = FindFirstChild(playerGui, "PHETAMINE");
    if (g_hiddenContainer) log::Info("walker: hidden container found at %p", (void*)g_hiddenContainer);
    return g_hiddenContainer;
}

}  // namespace phetamine::mem
