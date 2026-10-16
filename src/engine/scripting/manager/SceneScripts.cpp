#include "Internal.h"
#include "scripting/ComponentManager.h"
#include "scripting/ScriptReloadCandidate.h"
#include "shared/resources/ResourcePath.h"
#include <rapidjson/memorystream.h>
#include <rapidjson/encodings.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string_view>

using namespace ManagerDetail;
namespace {
namespace fs = std::filesystem;
using Ref = luabridge::LuaRef;
struct StackScope { lua_State *state; int top; ~StackScope() { lua_settop(state, top); } };
std::string PathText(const fs::path &path) {
    const auto text = path.generic_u8string(); return {text.begin(), text.end()};
}
bool UTF8(const std::string &value) {
    rapidjson::MemoryStream input(value.data(), value.size());
    while (input.Tell() < value.size()) { unsigned point = 0; if (!rapidjson::UTF8<>::Decode(input, &point)) return false; }
    return true;
}
void ValidPath(const fs::path &path) {
    const auto text = PathText(path);
    if (text.find('\0') != std::string::npos || !UTF8(text)) throw std::runtime_error("component source path must be valid UTF-8 without NUL");
}
bool TypeName(const std::string &type) {
    if (type.empty() || type.size() > 128) return false;
    const auto alpha = [](unsigned char c) { return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
    if (!alpha(type.front())) return false;
    for (unsigned char c : type) if (!alpha(c) && (c < '0' || c > '9')) return false;
    return true;
}
struct Selection { std::string type; fs::path path; std::string resource; };
std::vector<Selection> Discover(const fs::path &preferred) {
    ValidPath(preferred);
    if (preferred.is_absolute() || preferred.has_root_name() || PathText(preferred).find('\\') != std::string::npos)
        throw std::runtime_error("preferred component subdirectory must be relative");
    for (const auto &part : preferred) if (part == "..") throw std::runtime_error("preferred component subdirectory cannot escape its root");
    const auto root = fs::weakly_canonical(fs::absolute(ResourcePath::ResourceSubdirectory("component_types")));
    ValidPath(root);
    if (!fs::exists(root)) return {};
    if (!fs::is_directory(root)) throw std::runtime_error("component_types is not a directory");
    auto files = ResourcePath::CollectFilesRecursively(root, ".lua");
    ResourcePath::SortPathsWithPreference(files, root, preferred);
    std::unordered_set<std::string> selected;
    std::unordered_map<std::string, std::string> canonical_types;
    std::vector<Selection> result;
    for (const auto &file : files) {
        ValidPath(file);
        const auto type = PathText(file.stem());
        if (IsBuiltinComponentType(type) || !selected.insert(type).second) continue;
        if (Runtime().reserved_component_type_names.contains(type))
            throw std::runtime_error("component filename conflicts with reserved host/standard-library global: " + type);
        if (!TypeName(type)) throw std::runtime_error("invalid component filename/type: " + type);
        const auto canonical = fs::canonical(file);
        ValidPath(canonical);
        if (!ResourcePath::IsPathWithinDirectory(canonical, root)) throw std::runtime_error("selected component source escapes component_types: " + type);
        if (canonical.extension() != ".lua") throw std::runtime_error("canonical component source must end in .lua: " + type);
        const auto [owner, inserted] = canonical_types.emplace(PathText(canonical), type);
        if (!inserted && owner->second != type)
            throw std::runtime_error("one canonical component source cannot define selected types '" + owner->second + "' and '" + type + "'");
        result.push_back({type, canonical, PathText(file.lexically_relative(root))});
    }
    return result;
}
std::string Read(const fs::path &path) {
    if (!fs::is_regular_file(path)) throw std::runtime_error("component source is not a regular file");
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("cannot read component source");
    constexpr std::size_t maximum = 1024 * 1024;
    std::string text(maximum + 1, '\0');
    stream.read(text.data(), static_cast<std::streamsize>(text.size()));
    const auto size = static_cast<std::size_t>(stream.gcount());
    if (size > maximum) throw std::runtime_error("component source exceeds 1 MiB");
    if (stream.bad()) throw std::runtime_error("component source read failed");
    text.resize(size);
    if (text.find('\0') != std::string::npos || !UTF8(text)) throw std::runtime_error("component source must be UTF-8 text without NUL");
    return text;
}
Ref Global(lua_State *state, const std::string &name) {
    StackScope stack{state, lua_gettop(state)};
    lua_pushglobaltable(state); lua_pushlstring(state, name.data(), name.size()); lua_rawget(state, -2);
    return Ref::fromStack(state, -1);
}
std::string LuaError(lua_State *state) {
    if (lua_type(state, -1) != LUA_TSTRING) return "non-string component error";
    std::size_t size = 0; const char *text = lua_tolstring(state, -1, &size);
    return ScriptReloadCandidate::SanitizeDiagnosticText({text, size});
}
} // namespace

namespace ManagerDetail {
void PauseScenePreparationGC(lua_State *state) {
    if (!state || state != Runtime().lua_state) throw std::logic_error("scene preparation GC guard requires the current live Lua VM");
    if (Runtime().scene_script_gc_guards++ == 0) {
        Runtime().scene_script_gc_was_running = lua_gc(state, LUA_GCISRUNNING) != 0;
        lua_gc(state, LUA_GCSTOP);
    }
}
void ResumeScenePreparationGC(lua_State *state) noexcept {
    if (!state || state != Runtime().lua_state || Runtime().scene_script_gc_guards == 0) return;
    if (--Runtime().scene_script_gc_guards == 0) {
        if (Runtime().scene_script_gc_was_running) lua_gc(state, LUA_GCRESTART);
        Runtime().scene_script_gc_was_running = false;
    }
}
} // namespace ManagerDetail

struct SceneScriptPlan::Impl {
    lua_State *state;
    std::unordered_map<std::string, Ref> types;
    std::unordered_map<std::string, std::string> sources;
    std::unordered_map<std::string, Ref> source_cache;
    std::unordered_map<std::string, std::string> source_owners;
    std::vector<std::string> changed;
    bool committed = false;
    explicit Impl(lua_State *value) : state(value) {
        PauseScenePreparationGC(state);
    }
    ~Impl() {
        // Drop every owned LuaRef before allowing collection/finalizers again.
        types.clear();
        source_cache.clear();
        ResumeScenePreparationGC(state);
    }
};
SceneScriptPlan::SceneScriptPlan(lua_State *state) : impl_(std::make_unique<Impl>(state)) {}
SceneScriptPlan::~SceneScriptPlan() = default;
SceneScriptPlan::SceneScriptPlan(SceneScriptPlan &&) noexcept = default;
SceneScriptPlan &SceneScriptPlan::operator=(SceneScriptPlan &&) noexcept = default;

void ComponentManager::BootstrapComponentTypes(const fs::path &preferred) {
    if (!Runtime().lua_state) throw std::logic_error("Initialize must precede component bootstrap");
    if (Runtime().component_types_bootstrapped) return;
    const auto selections = Discover(preferred); // Select winners before any file executes.
    Runtime().component_types_bootstrapped = true;
    auto *state = Runtime().lua_state;
    for (const auto &selection : selections) {
        StackScope stack{state, lua_gettop(state)};
        try {
            const auto code = Read(selection.path);
            const auto chunk = "@" + selection.resource;
            if (luaL_loadbufferx(state, code.data(), code.size(), chunk.c_str(), "t") != LUA_OK || lua_pcall(state, 0, 0, 0) != LUA_OK)
                throw std::runtime_error(LuaError(state));
            auto prototype = Global(state, selection.type);
            if (!prototype.isTable()) throw std::runtime_error("expected global table named " + selection.type);
            const auto path = PathText(selection.path);
            Runtime().component_source_cache.insert_or_assign(path, prototype);
            Runtime().component_source_owners.emplace(path, selection.type);
            Runtime().component_type_tables.emplace(selection.type, std::move(prototype));
            Runtime().component_type_sources.emplace(selection.type, path);
        } catch (const std::exception &error) {
            LogComponentTypeWarningOnce("bootstrap:" + PathText(selection.path), ScriptReloadCandidate::SanitizeDiagnosticText(
                "problem with lua component [" + selection.type + "]: " + error.what()));
        }
    }
}

std::unique_ptr<SceneScriptPlan> ComponentManager::PrepareSceneScripts(const fs::path &preferred,
    const std::vector<Actor> &incoming, const std::vector<Actor::UID> &persistent_uids, std::string &error) {
    error.clear();
    std::unique_ptr<SceneScriptPlan> plan;
    try {
        if (!Runtime().lua_state || !Runtime().component_types_bootstrapped) throw std::logic_error("component scripts must bootstrap before scene preparation");
        plan.reset(new SceneScriptPlan(Runtime().lua_state));
        auto &prepared = *plan->impl_;
        prepared.types = Runtime().component_type_tables;
        prepared.sources = Runtime().component_type_sources;
        prepared.source_cache = Runtime().component_source_cache;
        prepared.source_owners = Runtime().component_source_owners;
        const auto selections = Discover(preferred);
        std::unordered_set<std::string> available;
        for (const auto &[name, unused] : prepared.types) available.insert(name);
        for (const auto &selection : selections) available.insert(selection.type);
        for (const auto &actor : incoming) for (const auto &component : actor.component_specs)
            if (!IsBuiltinComponentType(component.type) && !available.contains(component.type))
                throw std::runtime_error("missing Lua component type '" + component.type + "' for actor " + std::to_string(actor.uid));
        std::unordered_set<std::string> persistent_types;
        for (const auto uid : persistent_uids) {
            const auto actor = Runtime().actor_components.find(uid);
            if (actor == Runtime().actor_components.end()) continue;
            for (const auto &component : actor->second) if (!component->removed && !IsBuiltinComponentType(component->type)) persistent_types.insert(component->type);
        }
        std::vector<ScriptReloadCandidate::Source> candidates;
        for (const auto &selection : selections) {
            const auto existing = prepared.sources.find(selection.type);
            const auto canonical = PathText(selection.path);
            const auto owner = prepared.source_owners.find(canonical);
            if (owner != prepared.source_owners.end() && owner->second != selection.type)
                throw std::runtime_error("canonical component source already belongs to type '" + owner->second + "', not '" + selection.type + "'");
            if (existing != prepared.sources.end() && existing->second == canonical && prepared.types.contains(selection.type)) continue;
            if (persistent_types.contains(selection.type)) throw std::runtime_error("persistent actor holds component type '" + selection.type + "' whose selected source would change");
            prepared.sources[selection.type] = canonical;
            prepared.changed.push_back(selection.type);
            const auto cached = prepared.source_cache.find(canonical);
            if (cached != prepared.source_cache.end()) {
                if (owner == prepared.source_owners.end()) throw std::runtime_error("cached component source has no type owner");
                prepared.types.insert_or_assign(selection.type, cached->second);
            }
            else candidates.push_back({selection.type, selection.resource, Read(selection.path)});
        }
        if (!candidates.empty()) {
            auto staged = ScriptReloadCandidate::Stage(Runtime().lua_state, candidates);
            if (!staged.ok) {
                if (staged.diagnostics.empty()) throw std::runtime_error("candidate script preparation failed");
                const auto &diagnostic = staged.diagnostics.front();
                throw std::runtime_error(diagnostic.type + " [" + diagnostic.stage + "]: " + diagnostic.message);
            }
            for (auto &[name, value] : staged.exports) {
                prepared.source_cache.insert_or_assign(prepared.sources.at(name), value);
                prepared.source_owners.emplace(prepared.sources.at(name), name);
                prepared.types.insert_or_assign(name, std::move(value));
            }
        }
        return plan;
    } catch (const std::exception &failure) {
        error = ScriptReloadCandidate::SanitizeDiagnosticText(failure.what());
        plan.reset();
        return nullptr;
    }
}

luabridge::LuaRef ComponentManager::GetPreparedSceneType(const SceneScriptPlan &plan, const std::string &type) {
    if (!plan.impl_ || plan.impl_->state != Runtime().lua_state) throw std::logic_error("scene script plan belongs to another Lua lifetime");
    const auto &types = plan.impl_->committed ? Runtime().component_type_tables : plan.impl_->types;
    const auto found = types.find(type);
    return found == types.end() ? Ref(Runtime().lua_state) : found->second;
}
void ComponentManager::CommitSceneScripts(SceneScriptPlan &plan) {
    if (!plan.impl_ || plan.impl_->state != Runtime().lua_state || plan.impl_->committed) throw std::logic_error("scene script plan cannot be committed");
    auto &prepared = *plan.impl_;
    auto *state = prepared.state;
    StackScope stack{state, lua_gettop(state)};
    lua_pushglobaltable(state);
    for (const auto &type : prepared.changed) {
        lua_pushlstring(state, type.data(), type.size()); prepared.types.at(type).push(state); lua_rawset(state, -3);
    }
    Runtime().component_type_tables.swap(prepared.types);
    Runtime().component_type_sources.swap(prepared.sources);
    Runtime().component_source_cache.swap(prepared.source_cache);
    Runtime().component_source_owners.swap(prepared.source_owners);
    prepared.committed = true;
}
