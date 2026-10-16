#include "Internal.h"
#include "scripting/ComponentManager.h"
#include "scripting/EventBus.h"
#include "scripting/ScriptReloadCandidate.h"
#include "shared/resources/ResourcePath.h"
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <unordered_set>

using namespace ManagerDetail;
namespace {
using Ref=luabridge::LuaRef;
struct StackScope { lua_State *state;int top;~StackScope(){lua_settop(state,top);} };
struct GCStop {
    lua_State *state;bool running;
    explicit GCStop(lua_State *value):state(value),running(lua_gc(value,LUA_GCISRUNNING)!=0){lua_gc(state,LUA_GCSTOP);}
    ~GCStop(){if(running)lua_gc(state,LUA_GCRESTART);}
};
Ref Raw(const Ref &table,const char *name) {
    auto *state=Runtime().lua_state;StackScope stack{state,lua_gettop(state)};
    table.push(state);lua_pushstring(state,name);lua_rawget(state,-2);return Ref::fromStack(state,-1);
}
const void *Identity(const Ref &value) {
    auto *state=Runtime().lua_state;StackScope stack{state,lua_gettop(state)};
    value.push(state);return lua_topointer(state,-1);
}
bool Plain(const Ref &table) {
    auto *state=Runtime().lua_state;StackScope stack{state,lua_gettop(state)};
    table.push(state);return lua_getmetatable(state,-1)==0;
}
std::string Read(const std::filesystem::path &path) {
    std::ifstream input(path,std::ios::binary);
    if(!input)throw std::runtime_error("cannot read registered component source");
    constexpr std::size_t limit=1024*1024;
    std::string text(limit+1,'\0');input.read(text.data(),static_cast<std::streamsize>(text.size()));
    const auto size=static_cast<std::size_t>(input.gcount());
    if(size>limit)throw std::runtime_error("component source exceeds 1 MiB");
    if(input.bad())throw std::runtime_error("component source read failed");
    text.resize(size);return text;
}
constexpr std::array<const char *,8> callbacks={"OnStart","OnDestroy","OnUpdate","OnLateUpdate",
    "OnCollisionEnter","OnCollisionExit","OnTriggerEnter","OnTriggerExit"};
constexpr std::array<bool ComponentRecord::*,8> flags={&ComponentRecord::has_on_start,&ComponentRecord::has_on_destroy,
    &ComponentRecord::has_on_update,&ComponentRecord::has_on_late_update,&ComponentRecord::has_on_collision_enter,
    &ComponentRecord::has_on_collision_exit,&ComponentRecord::has_on_trigger_enter,&ComponentRecord::has_on_trigger_exit};
void ValidatePrototype(const Ref &prototype) {
    if(!prototype.isTable()||!Plain(prototype))throw std::runtime_error("reload requires a plain component prototype table");
    if(!Raw(prototype,"actor").isNil()||!Raw(prototype,"key").isNil())
        throw std::runtime_error("prototype cannot replace engine actor/key identity");
    auto *state=Runtime().lua_state;StackScope stack{state,lua_gettop(state)};
    prototype.push(state);lua_pushnil(state);
    while(lua_next(state,-2)) {
        if(lua_type(state,-2)!=LUA_TSTRING)throw std::runtime_error("prototype keys must be strings");
        std::size_t size=0;const char *key=lua_tolstring(state,-2,&size);
        if(size==0||size>256||std::string_view(key,size).find('\0')!=std::string_view::npos)
            throw std::runtime_error("invalid prototype key");
        lua_pop(state,1);
    }
    for(const char *name:callbacks) {
        auto value=Raw(prototype,name);
        if(!value.isNil()&&!value.isFunction())throw std::runtime_error(std::string(name)+" must be a function or nil");
    }
}
void ValidateInstanceMetatable(const Ref &instance,const Ref &prototype) {
    auto *state=Runtime().lua_state;StackScope stack{state,lua_gettop(state)};instance.push(state);
    if(!lua_getmetatable(state,-1))throw std::runtime_error("instance no longer has the engine prototype metatable");
    const int meta=lua_gettop(state);int entries=0;lua_pushnil(state);
    while(lua_next(state,meta)) {
        if(lua_type(state,-2)!=LUA_TSTRING||std::string(lua_tostring(state,-2))!="__index")
            throw std::runtime_error("custom instance metatable cannot be safely reloaded");
        if(!AreSameLuaRef(Ref::fromStack(state,-1),prototype))
            throw std::runtime_error("instance prototype differs from its registered type");
        ++entries;lua_pop(state,1);
    }
    if(entries!=1)throw std::runtime_error("invalid engine prototype metatable");
}
// Instance state remains the same object graph. Code embedded in that graph
// cannot be inferred to be a replaceable method; reject rather than retain a
// hidden old closure. Native values and references to other registered components keep
// their identity and are not traversed as if owned script tables.
void ValidateState(const Ref &root,const std::unordered_set<const void *> &instances) {
    auto *state=Runtime().lua_state;StackScope stack{state,lua_gettop(state)};
    std::unordered_set<const void *> seen;
    struct Entry{Ref value;int depth;bool root;};std::vector<Entry> pending{{root,0,true}};
    std::size_t nodes=0;
    while(!pending.empty()) {
        Entry entry=std::move(pending.back());pending.pop_back();
        if(++nodes>100000||entry.depth>64)throw std::runtime_error("instance state exceeds reload traversal limit");
        if(entry.value.isFunction())throw std::runtime_error("instance state contains a function; keep reloadable methods on the prototype");
        entry.value.push(state);const int value=lua_gettop(state);const int type=lua_type(state,value);
        if(type==LUA_TTHREAD||type==LUA_TLIGHTUSERDATA)throw std::runtime_error("instance state contains an unsupported continuation or pointer");
        if(type!=LUA_TTABLE){lua_pop(state,1);continue;}
        const void *id=lua_topointer(state,value);
        if(!seen.insert(id).second||(!entry.root&&instances.contains(id))){lua_pop(state,1);continue;}
        lua_pushnil(state);
        while(lua_next(state,value)) {
            pending.push_back({Ref::fromStack(state,-2),entry.depth+1,false});
            pending.push_back({Ref::fromStack(state,-1),entry.depth+1,false});lua_pop(state,1);
        }
        if(!entry.root&&lua_getmetatable(state,value)) {
            pending.push_back({Ref::fromStack(state,-1),entry.depth+1,false});lua_pop(state,1);
        }
        lua_pop(state,1);
    }
}
std::vector<EventBus::FunctionReplacement> Methods(const Ref &before,const Ref &after) {
    auto *state=Runtime().lua_state;StackScope stack{state,lua_gettop(state)};
    std::vector<EventBus::FunctionReplacement> result;
    before.push(state);const int index=lua_gettop(state);lua_pushnil(state);
    while(lua_next(state,index)) {
        if(lua_isfunction(state,-1)) {
            const std::string name=lua_tostring(state,-2);
            auto next=Raw(after,name.c_str());
            if(!next.isNil()&&!next.isFunction())throw std::runtime_error("a method cannot change into instance data during reload");
            result.push_back({Ref::fromStack(state,-1),std::move(next)});
        }
        lua_pop(state,1);
    }
    return result;
}
struct Prepared {
    ComponentRecord *record;
    Ref meta;
    Ref defaults;
    std::array<bool,8> callbacks;
};
} // namespace

std::string ComponentManager::GetComponentScriptSource(const std::string &type) {
    if(IsBuiltinComponentType(type))return {};
    const auto found=Runtime().component_type_sources.find(type);
    return found==Runtime().component_type_sources.end()?std::string{}:found->second;
}

ScriptReloadResult ComponentManager::ReloadScripts(const std::vector<std::string> &types) {
    ScriptReloadResult result;auto *state=Runtime().lua_state;
    auto fail=[&](const std::string &type,const std::string &resource,const std::string &stage,const std::string &message){
        result.diagnostics.push_back({ScriptReloadCandidate::SanitizeDiagnosticText(type),
            ScriptReloadCandidate::SanitizeDiagnosticText(resource),stage,
            ScriptReloadCandidate::SanitizeDiagnosticText(message),0});return result;
    };
    if(!state)return fail("","","validate","Lua runtime is not initialized");
    lua_Debug activation{};
    if(EventBus::IsDispatching()||lua_getstack(state,0,&activation))
        return fail("","","busy","script reload requires an idle host boundary");
    if(types.empty()||types.size()>64)return fail("","","validate","reload requires 1..64 component types");
    StackScope stack{state,lua_gettop(state)};GCStop gc(state);
    std::string current_type,current_resource;
    bool commit_started=false;
    try {
        std::unordered_set<std::string> unique;
        std::vector<ScriptReloadCandidate::Source> sources;
        const auto root=std::filesystem::weakly_canonical(ResourcePath::ResourceSubdirectory("component_types"));
        for(const auto &type:types) {
            current_type=type;current_resource.clear();
            if(type.empty()||type.size()>128||type.find('\0')!=std::string::npos||!unique.insert(type).second)
                return fail(type,"","validate","type names must be nonempty, bounded and unique");
            auto found=Runtime().component_type_tables.find(type);
            auto location=Runtime().component_type_sources.find(type);
            if(IsBuiltinComponentType(type)||found==Runtime().component_type_tables.end()||location==Runtime().component_type_sources.end())
                return fail(type,"","validate","type is not a registered Lua component");
            ValidatePrototype(found->second);
            const auto registered=std::filesystem::path(std::u8string(location->second.begin(),location->second.end()));
            const auto path=std::filesystem::weakly_canonical(registered);
            if(path!=registered.lexically_normal())
                return fail(type,"","read","registered canonical component source was redirected; select the new source through a scene transition");
            if(!ResourcePath::IsPathWithinDirectory(path,root)||path.extension()!=".lua")
                return fail(type,"","read","registered source is outside the component directory");
            current_resource=std::filesystem::relative(path,ResourcePath::ResourcesRootPath()).generic_string();
            try { sources.push_back({type,current_resource,Read(path)}); }
            catch(const std::exception &error){return fail(type,current_resource,"read",error.what());}
        }
        std::sort(sources.begin(),sources.end(),[](const auto &a,const auto &b){return a.resource<b.resource;});
        auto candidate=ScriptReloadCandidate::Stage(state,sources);
        if(!candidate.ok) {
            for(const auto &error:candidate.diagnostics)result.diagnostics.push_back({error.type,error.resource,error.stage,error.message,error.line});
            return result;
        }
        auto registry=Runtime().component_type_tables;
        auto source_cache=Runtime().component_source_cache;
        std::unordered_map<std::string,std::vector<EventBus::FunctionReplacement>> replacements;
        for(const auto &source:sources) {
            current_type=source.type;current_resource=source.resource;
            const auto &prototype=candidate.exports.at(source.type);ValidatePrototype(prototype);
            replacements.emplace(source.type,Methods(registry.at(source.type),prototype));
            registry.at(source.type)=prototype;
            source_cache.insert_or_assign(Runtime().component_type_sources.at(source.type),prototype);
            result.types.push_back(source.type);
        }
        std::unordered_set<const void *> instances;
        for(const auto &[uid,records]:Runtime().actor_components)for(const auto &record:records)
            if(record->instance_table.isTable())instances.insert(Identity(record->instance_table));
        std::vector<Prepared> prepared;
        std::vector<EventBus::ComponentReload> events;
        std::unordered_map<ComponentRecord *,std::array<bool,8>> future_flags;
        for(const auto &[uid,records]:Runtime().actor_components)for(const auto &record:records) {
            if(record->removed||!unique.contains(record->type))continue;
            current_type=record->type;current_resource=std::filesystem::relative(Runtime().component_type_sources.at(current_type),ResourcePath::ResourcesRootPath()).generic_string();
            if(record->on_destroy_called||Runtime().pending_destroy_actor_uids.contains(uid))
                return fail(current_type,current_resource,"validate","cannot reload a component pending destruction");
            const auto &before=Runtime().component_type_tables.at(record->type);
            const auto &after=candidate.exports.at(record->type);
            ValidateInstanceMetatable(record->instance_table,before);ValidateState(record->instance_table,instances);
            auto defaults=CloneComponentDefaults(after,record->type,&record->instance_table);ValidateState(defaults,instances);
            // Existing field data never silently shadows a newly introduced method.
            after.push(state);lua_pushnil(state);
            while(lua_next(state,-2)) {
                if(lua_isfunction(state,-1)) {
                    const auto existing=Raw(record->instance_table,lua_tostring(state,-2));
                    if(!existing.isNil())throw std::runtime_error("new method conflicts with an existing instance field");
                }
                lua_pop(state,1);
            }
            lua_pop(state,1);
            auto meta=luabridge::newTable(state);meta["__index"]=after;
            std::array<bool,8> callbacks_present{};
            for(std::size_t i=0;i<callbacks.size();++i)callbacks_present[i]=Raw(after,callbacks[i]).isFunction();
            prepared.push_back({record.get(),std::move(meta),std::move(defaults),callbacks_present});
            future_flags.emplace(record.get(),callbacks_present);
            events.push_back({record->instance_table,replacements.at(record->type)});
        }
        EventBus::ReloadPlan event_plan;std::string event_error;
        if(!EventBus::PrepareReload(events,event_plan,event_error))return fail(current_type,current_resource,"validate",event_error);
        // All C++ allocations and lifecycle ordering happen before publication.
        std::vector<LifecycleComponentRef> updates,late_updates;
        const std::unordered_set<Actor::UID> pending_actors(Runtime().pending_actor_uids_to_activate.begin(),Runtime().pending_actor_uids_to_activate.end());
        for(const auto &[uid,records]:Runtime().actor_components) {
            if(Runtime().pending_destroy_actor_uids.contains(uid)||pending_actors.contains(uid))continue;
            for(const auto &record:records) {
                if(record->removed)continue;
                const auto found=future_flags.find(record.get());
                const bool update=found==future_flags.end()?record->has_on_update:found->second[2];
                const bool late=found==future_flags.end()?record->has_on_late_update:found->second[3];
                if(update)updates.emplace_back(uid,record.get());if(late)late_updates.emplace_back(uid,record.get());
            }
        }
        std::sort(updates.begin(),updates.end(),CompareLifecycleComponentRef);
        std::sort(late_updates.begin(),late_updates.end(),CompareLifecycleComponentRef);
        // Commit uses raw Lua writes and no user callbacks. Main-VM allocator
        // exhaustion remains the host's fatal/reset boundary, not a recoverable
        // rejection with an invented rollback guarantee.
        commit_started=true;
        for(auto &entry:prepared) {
            entry.record->instance_table.push(state);const int instance=lua_gettop(state);
            entry.defaults.push(state);const int defaults=lua_gettop(state);lua_pushnil(state);
            while(lua_next(state,defaults)) {
                lua_pushvalue(state,-2);lua_rawget(state,instance);const bool absent=lua_isnil(state,-1);lua_pop(state,1);
                if(absent){lua_pushvalue(state,-2);lua_pushvalue(state,-2);lua_rawset(state,instance);}
                lua_pop(state,1);
            }
            lua_pop(state,1);entry.meta.push(state);lua_setmetatable(state,instance);lua_pop(state,1);
            for(std::size_t i=0;i<flags.size();++i)entry.record->*flags[i]=entry.callbacks[i];
        }
        for(const auto &source:sources) {
            lua_pushglobaltable(state);lua_pushlstring(state,source.type.data(),source.type.size());
            candidate.exports.at(source.type).push(state);lua_rawset(state,-3);lua_pop(state,1);
        }
        Runtime().component_type_tables.swap(registry);
        Runtime().component_source_cache.swap(source_cache);
        Runtime().on_update_components.swap(updates);Runtime().on_late_update_components.swap(late_updates);
        EventBus::CommitReload(std::move(event_plan));
        result.instances_updated=static_cast<int>(prepared.size());result.ok=true;return result;
    }catch(const std::exception &error){if(commit_started)throw;return fail(current_type,current_resource,"validate",error.what());}
}
