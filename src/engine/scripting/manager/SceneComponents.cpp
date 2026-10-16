#include "scripting/LuaBoundary.h"
#include "scripting/SceneComponentPlan.h"
#include "Internal.h"
#include "scripting/ComponentManager.h"
#include "particles/ParticleSystem.h"
#include "physics/Rigidbody.h"
#include "rendering/SpriteAnimation.h"
#include "rendering/SpriteRenderer.h"
#include "scene/ActorHandle.h"
#include "scene/Tilemap.h"
#include "scene/Transform.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace {
using Ref=luabridge::LuaRef;
using namespace ManagerDetail;
struct StackScope { lua_State *state;int top;~StackScope(){lua_settop(state,top);} };
constexpr std::array<const char *,8> callbacks={"OnStart","OnDestroy","OnUpdate","OnLateUpdate",
    "OnCollisionEnter","OnCollisionExit","OnTriggerEnter","OnTriggerExit"};
void Require(bool value,const std::string &message){if(!value)throw std::runtime_error(message);}
bool Number(const Actor::ComponentPropertyValue &value,double &result) {
    if(const auto *number=std::get_if<int>(&value)){result=*number;return true;}
    if(const auto *number=std::get_if<double>(&value)){result=*number;return true;}
    return false;
}
void ValidateOverrides(const Actor::ComponentSpec &spec) {
    Require(!spec.key.empty() && spec.key.size()<=256 && spec.key.find('\0')==std::string::npos,"invalid component key");
    Require(!spec.type.empty() && spec.type.size()<=128 && spec.type.find('\0')==std::string::npos,"invalid component type");
    const bool native=IsBuiltinComponentType(spec.type);
    const auto schema=native?ComponentManager::GetComponentTypeDefaultProperties(spec.type):std::vector<Actor::ComponentProperty>{};
    std::unordered_set<std::string> fields;
    for(const auto &property:spec.overrides) {
        Require(!property.name.empty() && property.name.size()<=256 && property.name.find('\0')==std::string::npos,"invalid component property name");
        Require(fields.insert(property.name).second,"duplicate component property '"+property.name+"'");
        Require(property.name!="actor" && property.name!="key","component actor/key are engine-owned");
        if(!native) continue;
        const auto found=std::find_if(schema.begin(),schema.end(),[&](const auto &entry){return entry.name==property.name;});
        Require(found!=schema.end(),"unknown or readonly "+spec.type+" property '"+property.name+"'");
        if(std::holds_alternative<bool>(found->value)) {
            Require(std::holds_alternative<bool>(property.value),property.name+" must be boolean");
        } else if(std::holds_alternative<std::string>(found->value)) {
            const auto *text=std::get_if<std::string>(&property.value);
            Require(text && text->find('\0')==std::string::npos,property.name+" must be a string without NUL");
        } else if(std::holds_alternative<Actor::DoubleArray>(found->value)) {
            const auto *numbers=std::get_if<Actor::DoubleArray>(&property.value);
            const auto *integers=std::get_if<Actor::IntArray>(&property.value);
            Require(numbers||integers,property.name+" must be an array of numbers");
            if(numbers)for(const double value:*numbers)
                Require(std::isfinite(value)&&std::abs(value)<=std::numeric_limits<float>::max(),property.name+" must hold finite float values");
        } else {
            double number=0;Require(Number(property.value,number),property.name+" must be numeric");
            if(spec.type=="ParticleSystem" && property.name=="end_scale" && std::isnan(number)) continue;
            Require(std::isfinite(number),property.name+" must be finite");
            if(std::holds_alternative<int>(found->value))
                Require(std::trunc(number)==number && number>=std::numeric_limits<int>::min() && number<=std::numeric_limits<int>::max(),property.name+" must fit an integer");
            else Require(std::abs(number)<=std::numeric_limits<float>::max(),property.name+" exceeds finite float range");
        }
    }
}
Ref StaticField(const Ref &object,const char *name) {
    auto *state=Runtime().lua_state;StackScope stack{state,lua_gettop(state)};
    if(!object.isTable()) return ProtectedField(object,name); // trusted native userdata binding
    object.push(state);std::unordered_set<const void *> visited;
    for(int depth=0;depth<1024;++depth) {
        Require(visited.insert(lua_topointer(state,-1)).second,"cyclic static lifecycle prototype chain");
        lua_pushstring(state,name);lua_rawget(state,-2);
        if(!lua_isnil(state,-1)) return Ref::fromStack(state,-1);
        lua_pop(state,1);
        if(!lua_getmetatable(state,-1)) return Ref(state);
        lua_pushliteral(state,"__index");lua_rawget(state,-2);lua_remove(state,-2);
        // Dynamic data and animation-event lookup remain supported at runtime;
        // construction never executes a function to synthesize core lifecycle methods.
        if(!lua_istable(state,-1)) return Ref(state);
        lua_replace(state,stack.top+1);
    }
    throw std::runtime_error("static lifecycle prototype chain exceeds 1024 tables");
}
}
namespace ManagerDetail {
std::unique_ptr<ComponentRecord> PrepareComponentRecord(const Actor::ComponentSpec &spec,const Ref &prototype) {
    ValidateOverrides(spec);
    auto *state=Runtime().lua_state;StackScope stack{state,lua_gettop(state)};
    Ref instance(state);
    if(spec.type=="Transform") instance=Ref(state,Transform{});
    else if(spec.type=="Rigidbody") instance=Ref(state,Rigidbody{});
    else if(spec.type=="ParticleSystem") instance=Ref(state,ParticleSystem{});
    else if(spec.type=="SpriteRenderer") instance=Ref(state,SpriteRenderer{});
    else if(spec.type=="SpriteAnimation") instance=Ref(state,SpriteAnimation{});
    else if(spec.type=="Tilemap") instance=Ref(state,Tilemap{});
    else {
        Require(prototype.isTable(),"missing Lua component prototype '"+spec.type+"'");
        instance=CloneComponentDefaults(prototype,spec.type);
        auto parent=prototype;EstablishInheritance(instance,parent);
    }
    SetSystemField(instance,"key",Ref(state,spec.key));
    SetSystemField(instance,"enabled",Ref(state,true));
    for(const auto &property:spec.overrides) {
        if(spec.type=="Tilemap" && property.name=="map_resource") LuaBoundary::Cast<Tilemap *>(instance)->map_resource=std::get<std::string>(property.value);
        else if(spec.type=="SpriteAnimation" && property.name!="enabled") {
            auto *animation=LuaBoundary::Cast<SpriteAnimation *>(instance);const auto &text=std::get<std::string>(property.value);
            if(property.name=="animation_resource") animation->animation_resource=text;
            else if(property.name=="renderer_key") animation->renderer_key=text;
            else animation->clip=text;
        } else AssignPropertyValueToLuaField(instance,property.name,property.value);
    }
    if(spec.type=="Rigidbody") LuaBoundary::Cast<Rigidbody *>(instance)->ValidateConfiguration();
    if(spec.type=="ParticleSystem") LuaBoundary::Cast<ParticleSystem *>(instance)->ValidateConfiguration();
    std::array<bool,8> present{};
    for(std::size_t index=0;index<callbacks.size();++index) present[index]=StaticField(instance,callbacks[index]).isFunction();
    // A type-level OnUpdateBatch also puts the instance in the update order.
    present[2]=present[2]||StaticField(instance,"OnUpdateBatch").isFunction();
    return std::make_unique<ComponentRecord>(spec.key,spec.type,std::move(instance),present[0],present[1],present[2],present[3],present[4],present[5],present[6],present[7]);
}
luabridge::LuaRef StaticLifecycleField(const luabridge::LuaRef &object, const char *name) {
    return StaticField(object, name);
}
void InstallPreparedComponent(Actor::UID uid,std::unique_ptr<ComponentRecord> record) {
    const auto actor=Runtime().actor_by_uid.find(uid);
    if(actor!=Runtime().actor_by_uid.end()) SetSystemField(record->instance_table,"actor",Ref(Runtime().lua_state,ActorHandle(actor->second)));
    const auto key=record->key;
    Runtime().actor_components[uid].push_back(std::move(record));
    Runtime().pending_on_start.emplace_back(uid,key);
    Runtime().pending_on_awake.emplace_back(uid,key);
    if(actor!=Runtime().actor_by_uid.end()) Runtime().dirty_component_actor_uids.insert(uid);
}
}
struct SceneComponentPlan::State {
    struct PreparedActor { Actor::UID uid;std::vector<std::unique_ptr<ManagerDetail::ComponentRecord>> records; };
    std::vector<PreparedActor> actors;
    bool installed=false;
};
SceneComponentPlan::SceneComponentPlan():state_(std::make_unique<State>()){}
SceneComponentPlan::~SceneComponentPlan()=default;
SceneComponentPlan::SceneComponentPlan(SceneComponentPlan &&)noexcept=default;
SceneComponentPlan &SceneComponentPlan::operator=(SceneComponentPlan &&)noexcept=default;
std::unique_ptr<SceneComponentPlan> PrepareSceneComponents(const std::vector<Actor> &actors,const SceneScriptPlan &scripts,std::string &error) {
    error.clear();std::string context;
    try {
        auto plan=std::unique_ptr<SceneComponentPlan>(new SceneComponentPlan());
        std::unordered_set<Actor::UID> uids;
        for(const auto &actor:actors) {
            Require(actor.uid!=Actor::kInvalidUID && uids.insert(actor.uid).second,"invalid or duplicate prepared actor uid");
            SceneComponentPlan::State::PreparedActor prepared{actor.uid,{}};
            std::unordered_set<std::string> keys;
            for(const auto &spec:actor.component_specs) {
                context="actor "+std::to_string(actor.uid)+" component '"+spec.key+"' ("+spec.type+"): ";
                Require(keys.insert(spec.key).second,"duplicate component key");
                const auto prototype=ManagerDetail::IsBuiltinComponentType(spec.type)?Ref(ManagerDetail::Runtime().lua_state):
                    ComponentManager::GetPreparedSceneType(scripts,spec.type);
                prepared.records.push_back(ManagerDetail::PrepareComponentRecord(spec,prototype));
            }
            std::sort(prepared.records.begin(),prepared.records.end(),[](const auto &a,const auto &b){return a->key<b->key;});
            plan->state_->actors.push_back(std::move(prepared));
        }
        // Resolve the incoming authored Transform graph with the same float
        // arithmetic as runtime, before any body can consume an infinite pose.
        // Parents outside this incoming set require the host's merged-world check.
        struct Pose {Actor::UID parent;float x,y,rotation;bool body=false,done=false;};
        std::unordered_map<Actor::UID,Pose> poses;
        for(std::size_t index=0;index<actors.size();++index) {
            Transform *transform=nullptr;bool body=false;
            for(const auto &record:plan->state_->actors[index].records) {
                if(record->type=="Transform" && !transform)transform=record->Native<Transform>();
                body |= record->type=="Rigidbody";
            }
            if(transform)poses.emplace(actors[index].uid,Pose{actors[index].parent_uid,transform->x,transform->y,transform->rotation,body,false});
        }
        for(const auto &[uid,unused]:poses) {
            std::vector<Actor::UID> path;std::unordered_set<Actor::UID> visiting;auto current=uid;
            while(poses.contains(current) && !poses.at(current).done) {
                Require(visiting.insert(current).second,"cyclic authored Transform hierarchy");
                path.push_back(current);current=poses.at(current).parent;
            }
            for(auto iterator=path.rbegin();iterator!=path.rend();++iterator) {
                auto &pose=poses.at(*iterator);
                const auto parent=poses.find(pose.parent);
                if(parent!=poses.end()) {
                    const float radians=parent->second.rotation*(3.14159265358979323846f/180.0f);
                    const float cosine=std::cos(radians),sine=std::sin(radians);
                    const float x=cosine*pose.x+sine*pose.y,y=-sine*pose.x+cosine*pose.y;
                    Require(std::isfinite(x)&&std::isfinite(y),"authored Transform rotation overflows finite float");
                    pose.x=parent->second.x+x;pose.y=parent->second.y+y;pose.rotation+=parent->second.rotation;
                }
                Require(std::isfinite(pose.x)&&std::isfinite(pose.y)&&std::isfinite(pose.rotation),"authored Transform world pose overflows finite float");
                if(pose.body)Require(std::abs(pose.x)<=1e8f&&std::abs(pose.y)<=1e8f,"Rigidbody world position exceeds supported physics range (+/-1e8)");
                pose.done=true;
            }
        }
        return plan;
    }catch(const std::exception &failure){error=context+failure.what();return nullptr;}
}
void InstallSceneComponents(SceneComponentPlan &plan,std::list<Actor> &actors,std::size_t begin) {
    Require(plan.state_ && !plan.state_->installed,"scene component plan was already installed");
    Require(begin<=actors.size() && actors.size()-begin==plan.state_->actors.size(),"scene component plan actor range mismatch");
    auto actor=actors.begin();std::advance(actor,begin);
    for(std::size_t index=0;index<plan.state_->actors.size();++index,++actor)
        Require(actor->uid==plan.state_->actors[index].uid,"scene component plan actor identity mismatch");
    // Register all prepared records before native initialization resolves peers.
    for(auto &entry:plan.state_->actors) {
        Require(ManagerDetail::Runtime().actor_components.find(entry.uid)==ManagerDetail::Runtime().actor_components.end(),"incoming actor collides with existing components");
        for(auto &record:entry.records) ManagerDetail::InstallPreparedComponent(entry.uid,std::move(record));
        ManagerDetail::SortComponentsForActor(entry.uid);
    }
    for(const auto &entry:plan.state_->actors) {
        const auto found=ManagerDetail::Runtime().actor_components.find(entry.uid);
        if(found!=ManagerDetail::Runtime().actor_components.end())
            for(const auto &record:found->second) if(record->type=="Tilemap") ComponentManager::InitializeTilemap(entry.uid,record->key);
        ComponentManager::InitializeSpriteAnimations(entry.uid);
    }
    plan.state_->installed=true;
}

struct PreparedRuntimeComponent::State {
    lua_State *state;
    std::unique_ptr<ManagerDetail::ComponentRecord> record;
    explicit State(lua_State *value):state(value){ManagerDetail::PauseScenePreparationGC(state);}
    ~State(){record.reset();ManagerDetail::ResumeScenePreparationGC(state);}
};
PreparedRuntimeComponent::PreparedRuntimeComponent(lua_State *state):state_(std::make_unique<State>(state)){}
PreparedRuntimeComponent::~PreparedRuntimeComponent()=default;
PreparedRuntimeComponent::PreparedRuntimeComponent(PreparedRuntimeComponent &&)noexcept=default;
PreparedRuntimeComponent &PreparedRuntimeComponent::operator=(PreparedRuntimeComponent &&)noexcept=default;
std::unique_ptr<PreparedRuntimeComponent> PrepareRuntimeComponent(const Actor::ComponentSpec &spec,std::string &error) {
    error.clear();
    try {
        auto *state=ManagerDetail::Runtime().lua_state;Require(state!=nullptr,"Lua runtime is not initialized");
        auto token=std::unique_ptr<PreparedRuntimeComponent>(new PreparedRuntimeComponent(state));
        Ref prototype(state);
        if(!ManagerDetail::IsBuiltinComponentType(spec.type)) {
            const auto found=ManagerDetail::Runtime().component_type_tables.find(spec.type);
            Require(found!=ManagerDetail::Runtime().component_type_tables.end(),"component type is not registered: "+spec.type);
            prototype=found->second;
        }
        token->state_->record=ManagerDetail::PrepareComponentRecord(spec,prototype);
        return token;
    }catch(const std::exception &failure){error="component '"+spec.key+"' ("+spec.type+"): "+failure.what();return nullptr;}
}
void InstallRuntimeComponent(PreparedRuntimeComponent &plan,Actor::UID uid) {
    Require(plan.state_ && plan.state_->record,"runtime component plan was already installed");
    Require(ManagerDetail::Runtime().actor_by_uid.contains(uid) && !ManagerDetail::Runtime().pending_destroy_actor_uids.contains(uid),"component owner is not live");
    Require(ManagerDetail::FindComponentRecord(uid,plan.state_->record->key)==nullptr,"component key still belongs to a live instance");
    const auto key=plan.state_->record->key;
    const auto type=plan.state_->record->type;
    ManagerDetail::InstallPreparedComponent(uid,std::move(plan.state_->record));
    ManagerDetail::SortComponentsForActor(uid);
    if(type=="Tilemap") ComponentManager::InitializeTilemap(uid,key);
    ComponentManager::InitializeSpriteAnimations(uid);
}
