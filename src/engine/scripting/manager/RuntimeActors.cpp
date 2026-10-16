#include "scripting/LuaBoundary.h"
#include "scripting/RuntimeActorPlan.h"
#include "scripting/SceneComponentPlan.h"
#include "scripting/ComponentManager.h"
#include "scripting/ScriptReloadCandidate.h"
#include "Internal.h"
#include "physics/Rigidbody.h"
#include "scene/Transform.h"
#include <rapidjson/encodings.h>
#include <rapidjson/memorystream.h>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_set>

namespace {
using namespace ManagerDetail;
void Require(bool value,const std::string &message) { if(!value) throw std::runtime_error(message); }
void Text(const std::string &value,const char *label) {
    Require(value.find('\0')==std::string::npos,std::string(label)+" contains NUL");
    rapidjson::MemoryStream input(value.data(),value.size());
    while(input.Tell()<value.size()) {
        unsigned point=0;
        Require(rapidjson::UTF8<>::Decode(input,&point),std::string(label)+" is not valid UTF-8");
    }
}
void UnusedUID(Actor::UID uid) {
    Require(!Runtime().actor_by_uid.contains(uid)&&!Runtime().actor_components.contains(uid)&&
            !Runtime().pending_destroy_actor_uids.contains(uid),"runtime actor UID is already registered");
}
void ValidatePoses(const std::vector<std::unique_ptr<ComponentRecord>> &records) {
    bool has_body=false;
    for(const auto &record:records) has_body|=record->type=="Rigidbody";
    for(const auto &record:records) {
        if(record->type!="Transform") continue;
        auto *transform=record->Native<Transform>();
        Require(std::isfinite(transform->x)&&std::isfinite(transform->y)&&std::isfinite(transform->rotation),
                "runtime actor Transform pose must be finite");
        if(has_body) {
            Rigidbody::ValidateNumericProperty("x",transform->x);
            Rigidbody::ValidateNumericProperty("y",transform->y);
            Rigidbody::ValidateNumericProperty("rotation",transform->rotation);
        }
        // Root actors have no inherited pose. Preserve the existing authored
        // Rigidbody pose contract rather than silently rewriting its x/y.
        transform->world_x=transform->x;
        transform->world_y=transform->y;
        transform->world_rotation=transform->rotation;
    }
}
} // namespace

struct PreparedRuntimeActor::State {
    lua_State *vm;
    std::list<Actor> node;
    std::vector<std::unique_ptr<ManagerDetail::ComponentRecord>> records;
    bool commit_started=false;
    bool published=false;
    explicit State(lua_State *state):vm(state) { ManagerDetail::PauseScenePreparationGC(state); }
    ~State() {
        records.clear(); node.clear();
        ManagerDetail::ResumeScenePreparationGC(vm);
    }
};
PreparedRuntimeActor::PreparedRuntimeActor(lua_State *state):state_(std::make_unique<State>(state)) {}
PreparedRuntimeActor::~PreparedRuntimeActor()=default;
PreparedRuntimeActor::PreparedRuntimeActor(PreparedRuntimeActor &&) noexcept=default;
PreparedRuntimeActor &PreparedRuntimeActor::operator=(PreparedRuntimeActor &&) noexcept=default;
bool PreparedRuntimeActor::CommitStarted() const noexcept { return state_&&state_->commit_started; }

std::unique_ptr<PreparedRuntimeActor> PrepareRuntimeActor(Actor candidate,std::string &error) {
    error.clear();
    std::string context;
    std::unique_ptr<PreparedRuntimeActor> plan;
    try {
        Require(Runtime().lua_state&&Runtime().scene_actors,"runtime actor preparation requires a bound live scene");
        Require(candidate.uid>=Actor::kRuntimeGeneratedUIDStart,"runtime actor requires a reserved generated UID");
        Require(candidate.runtime_generation==0,"unpublished runtime actor must have generation zero");
        Require(candidate.parent_uid==Actor::kInvalidUID,"new runtime actor must be a root");
        Require(!candidate.scene_backed&&!candidate.runtime_destroyed,"runtime actor candidate has invalid provenance/liveness");
        UnusedUID(candidate.uid);Text(candidate.actor_name,"actor name");
        plan.reset(new PreparedRuntimeActor(Runtime().lua_state));
        auto &prepared=*plan->state_;
        prepared.records.reserve(candidate.component_specs.size());
        std::sort(candidate.component_specs.begin(),candidate.component_specs.end(),[](const auto &a,const auto &b){return a.key<b.key;});
        std::unordered_set<std::string> keys;
        for(const auto &spec:candidate.component_specs) {
            context="component '"+spec.key+"' ("+spec.type+"): ";
            Text(spec.key,"component key");Text(spec.type,"component type");
            Require(keys.insert(spec.key).second,"duplicate runtime component key");
            luabridge::LuaRef prototype(Runtime().lua_state);
            if(!IsBuiltinComponentType(spec.type)) {
                const auto found=Runtime().component_type_tables.find(spec.type);
                Require(found!=Runtime().component_type_tables.end(),"component type is not registered");
                prototype=found->second;
            }
            prepared.records.push_back(PrepareComponentRecord(spec,prototype));
        }
        context="runtime actor pose/assets: ";
        ValidatePoses(prepared.records);
        prepared.node.emplace_back(std::move(candidate));
        PrepareDetachedActorAssets(prepared.node.front(),prepared.records);
        return plan;
    } catch(const std::exception &failure) {
        error=ScriptReloadCandidate::SanitizeDiagnosticText(context+failure.what());
        plan.reset();
        return nullptr;
    }
}

Actor *PublishRuntimeActor(PreparedRuntimeActor &plan,std::list<Actor> &actors,bool deferred_activation) {
    Require(plan.state_&&plan.state_->vm==Runtime().lua_state,"runtime actor token belongs to another Lua lifetime");
    auto &prepared=*plan.state_;
    Require(!prepared.commit_started&&!prepared.published&&prepared.node.size()==1,"runtime actor token was already published");
    Require(Runtime().scene_actors==&actors,"runtime actor target is not the bound scene container");
    Actor *actor=&prepared.node.front();
    UnusedUID(actor->uid);
    // Everything below may allocate host indexes or Lua identity userdata.
    // Callers must enter their fatal/reset path after an unexpected failure.
    prepared.commit_started=true;
    actor->AssignRuntimeGeneration();
    actors.splice(actors.end(),prepared.node);
    Runtime().actor_by_uid.emplace(actor->uid,actor);
    Runtime().actors_by_name[actor->actor_name].push_back(actor);
    Runtime().actor_order_by_uid[actor->uid]=Runtime().next_actor_order++;
    if(deferred_activation) Runtime().pending_actor_uids_to_activate.push_back(actor->uid);
    else Runtime().actor_uids_sorted.push_back(actor->uid);
    for(auto &record:prepared.records) InstallPreparedComponent(actor->uid,std::move(record));
    prepared.records.clear();
    SortComponentsForActor(actor->uid);
    if(!deferred_activation) {
        const auto found=Runtime().actor_components.find(actor->uid);
        if(found!=Runtime().actor_components.end()) for(const auto &record:found->second) {
            if(record->has_on_update) Runtime().on_update_components.emplace_back(actor->uid,record.get());
            if(record->has_on_late_update) Runtime().on_late_update_components.emplace_back(actor->uid,record.get());
        }
        Runtime().dirty_component_actor_uids.erase(actor->uid);
    }
    ApplyDetachedActorPoses(actor->uid);
    prepared.published=true;
    return actor;
}
