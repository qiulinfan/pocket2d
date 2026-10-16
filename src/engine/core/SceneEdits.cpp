#include "scripting/LuaBoundary.h"
#include "core/Engine.h"
#include "scripting/ComponentManager.h"
#include "scripting/EventBus.h"
#include "scripting/SceneComponentPlan.h"
#include "scripting/RuntimeActorAssets.h"
#include "scripting/ScriptReloadCandidate.h"
#include "scene/Scene.h"
#include "scene/Transform.h"
#include "scene/Tilemap.h"
#include "rendering/SpriteAnimation.h"
#include "physics/Rigidbody.h"
#include "physics/RayCast.h"
#include "../scripting/manager/Internal.h"
#include <rapidjson/encodings.h>
#include <rapidjson/memorystream.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>
#include <type_traits>
#include <unordered_set>

namespace {
using namespace ManagerDetail;
using UID=Actor::UID;
using Ref=luabridge::LuaRef;
using Spec=Actor::ComponentSpec;
struct Pose {float x=0,y=0,rotation=0;};
void Require(bool condition,const std::string &message) { if(!condition)throw std::runtime_error(message); }
void Text(const std::string &text,const char *label,bool allow_empty=false) {
    Require((allow_empty||!text.empty())&&text.find('\0')==std::string::npos,std::string("invalid ")+label);
    rapidjson::MemoryStream input(text.data(),text.size());
    while(input.Tell()<text.size()){unsigned cp=0;Require(rapidjson::UTF8<>::Decode(input,&cp),std::string(label)+" must be UTF-8");}
}
struct GCScope {
    lua_State *state=Runtime().lua_state;
    GCScope(){PauseScenePreparationGC(state);}
    ~GCScope(){ResumeScenePreparationGC(state);}
};
struct ComponentEdit {
    Spec spec, effective;
    std::string original_key, original_type;
    Ref original;
    ComponentRecord *original_record=nullptr;
    bool authored=true, removed=false, replace=false, changed=false, adopt=false;
    std::map<std::string,Actor::ComponentPropertyValue> patches;
    std::unique_ptr<ComponentRecord> prepared;
    explicit ComponentEdit(Spec value):spec(std::move(value)),effective(spec),original(Runtime().lua_state){}
};
struct ActorEdit {
    Actor candidate;
    Actor *original=nullptr;
    // A deferred/dead parent is already unavailable to runtime Transform lookup.
    // Model that original broken link as a root without publishing a parent edit.
    UID unavailable_original_parent=0;
    bool removed=false, changed=false, pose_changed=false;
    std::vector<std::unique_ptr<ComponentEdit>> components;
};
struct Plan {
    // Must outlive every Lua reference held below.
    GCScope gc;
    EventBus::KeyRemapPlan event_keys;
    std::unordered_map<UID,std::unique_ptr<ActorEdit>> entries;
    std::vector<UID> order;
    std::list<Actor> nodes;
    bool structural=false;
    // Local to one virtual mutation, never retained across a live edit. Each
    // parent edge is evaluated at most once until the virtual graph changes.
    std::unordered_map<UID,Pose> world_cache;
    std::unordered_map<UID,bool> affected_cache;
};
std::vector<Actor::ComponentProperty> NativeValues(UID uid,const std::string &key,const std::string &type) {
    auto values=ComponentManager::GetRuntimeComponentProperties(uid,key);
    const auto schema=ComponentManager::GetComponentTypeDefaultProperties(type);
    std::erase_if(values,[&](const auto &value){return std::none_of(schema.begin(),schema.end(),[&](const auto &field){return field.name==value.name;});});
    return values;
}
ActorEdit &ActorAt(Plan &plan,UID uid) {
    const auto found=plan.entries.find(uid);
    Require(found!=plan.entries.end()&&!found->second->removed,"scene edit actor does not exist: "+std::to_string(uid));
    return *found->second;
}
UID ParentOf(const ActorEdit &actor) {
    const UID parent=actor.candidate.parent_uid;
    return parent==actor.unavailable_original_parent?0:parent;
}
ComponentEdit *Find(ActorEdit &actor,const std::string &key) {
    for(auto &component:actor.components)if(!component->removed&&component->spec.key==key)return component.get();
    return nullptr;
}
ComponentEdit &At(ActorEdit &actor,const std::string &key) {
    auto *component=Find(actor,key);Require(component&&component->authored,"scene edit component does not exist: "+key);return *component;
}
ComponentEdit *Primary(ActorEdit &actor,const std::string &type) {
    ComponentEdit *result=nullptr;
    for(auto &component:actor.components)if(!component->removed&&component->spec.type==type&&(!result||component->spec.key<result->spec.key))result=component.get();
    return result;
}
double Number(const Spec &spec,const char *name,double fallback=0) {
    for(const auto &property:spec.overrides)if(property.name==name) {
        if(auto *value=std::get_if<int>(&property.value))return *value;
        if(auto *value=std::get_if<double>(&property.value))return *value;
        throw std::runtime_error(std::string(name)+" must be numeric");
    }
    return fallback;
}
Pose Local(ComponentEdit &transform) {
    return {static_cast<float>(Number(transform.effective,"x")),static_cast<float>(Number(transform.effective,"y")),static_cast<float>(Number(transform.effective,"rotation"))};
}
void Rotate(float x,float y,float degrees,float &out_x,float &out_y) {
    const float radians=degrees*0.017453292519943295769f;
    const float sine=std::sin(radians),cosine=std::cos(radians);
    out_x=x*cosine+y*sine;out_y=-x*sine+y*cosine;
}
Pose World(Plan &plan,UID uid) {
    std::vector<std::pair<UID,Pose>> path;std::unordered_set<UID> visited;
    Pose result;
    while(uid) {
        if(const auto cached=plan.world_cache.find(uid);cached!=plan.world_cache.end()){result=cached->second;break;}
        const auto found=plan.entries.find(uid);
        if(found==plan.entries.end()||found->second->removed)break;
        Require(visited.insert(uid).second,"scene edit creates a parent cycle");
        auto &actor=*found->second;auto *transform=Primary(actor,"Transform");
        if(!transform){plan.world_cache.emplace(uid,Pose{});break;}
        path.emplace_back(uid,Local(*transform));uid=ParentOf(actor);
    }
    for(auto it=path.rbegin();it!=path.rend();++it) {
        const auto &local=it->second;
        float x,y;Rotate(local.x,local.y,result.rotation,x,y);
        result={result.x+x,result.y+y,result.rotation+local.rotation};
        Require(std::isfinite(result.x)&&std::isfinite(result.y)&&std::isfinite(result.rotation),"scene edit world pose must be finite");
        plan.world_cache.emplace(it->first,result);
    }
    return result;
}
void Patch(ComponentEdit &component,const std::string &name,const Actor::ComponentPropertyValue &value) {
    Text(name,"component property");Require(name!="actor"&&name!="key","component actor/key are engine-owned");
    std::visit([&](const auto &entry) {
        using T=std::decay_t<decltype(entry)>;
        if constexpr(std::is_same_v<T,double>)Require(std::isfinite(entry)||(component.spec.type=="ParticleSystem"&&name=="end_scale"&&std::isnan(entry)),"component property must be finite");
        else if constexpr(std::is_same_v<T,std::string>)Text(entry,"component property string",true);
        else if constexpr(std::is_same_v<T,Actor::DoubleArray>)for(double number:entry)Require(std::isfinite(number),"component property array must be finite");
        else if constexpr(std::is_same_v<T,Actor::StringArray>)for(const auto &text:entry)Text(text,"component property string",true);
    },value);
    if(name=="enabled")Require(std::holds_alternative<bool>(value),"enabled must be boolean");
    SceneFormat::UpsertComponentProperty(component.spec,name,value);
    SceneFormat::UpsertComponentProperty(component.effective,name,value);
    component.patches[name]=value;component.changed=true;
    if(component.spec.type=="Rigidbody"||component.spec.type=="ParticleSystem")component.replace=true;
    if((component.spec.type=="Tilemap"||component.spec.type=="SpriteAnimation")&&name!="enabled")component.adopt=true;
}
void BuildSnapshot(Plan &plan,const std::list<Actor> &actors) {
    for(const auto &actor:actors) {
        if(actor.runtime_destroyed||Runtime().pending_destroy_actor_uids.contains(actor.uid))continue;
        auto item=std::make_unique<ActorEdit>();item->candidate=actor;item->original=const_cast<Actor *>(&actor);
        const auto found=Runtime().actor_components.find(actor.uid);
        if(found!=Runtime().actor_components.end())for(const auto &record:found->second) {
            if(record->removed)continue;
            const auto *authored=SceneFormat::FindComponentSpec(actor.component_specs,record->key);
            auto component=std::make_unique<ComponentEdit>(authored?*authored:Spec{record->key,record->type,{}});
            component->authored=authored!=nullptr;component->original_key=record->key;component->original_type=record->type;component->original=record->instance_table;component->original_record=record.get();
            if(IsBuiltinComponentType(record->type))component->effective.overrides=NativeValues(actor.uid,record->key,record->type);
            item->components.push_back(std::move(component));
        }
        plan.order.push_back(actor.uid);plan.entries.emplace(actor.uid,std::move(item));
    }
    for(auto &[uid,item]:plan.entries)if(item->candidate.parent_uid&&!plan.entries.contains(item->candidate.parent_uid))
        item->unavailable_original_parent=item->candidate.parent_uid;
}
void Simulate(Plan &plan,const SceneFormat::SceneMutation &mutation) {
    std::visit([&](const auto &edit) {
        using T=std::decay_t<decltype(edit)>;
        if constexpr(std::is_same_v<T,SceneFormat::CreateActorMutation>) {
            Require(edit.actor_uid!=0&&!plan.entries.contains(edit.actor_uid)&&!Runtime().actor_by_uid.contains(edit.actor_uid),"scene edit actor UID is already used or zero");
            auto position=plan.order.end();
            if(edit.insert_after_actor_uid){ActorAt(plan,*edit.insert_after_actor_uid);position=std::find(plan.order.begin(),plan.order.end(),*edit.insert_after_actor_uid)+1;}
            auto item=std::make_unique<ActorEdit>();item->candidate=SceneFormat::BuildEffectiveActor(edit.actor_record,Scene::GetActiveSceneSubdirectory());
            item->candidate.uid=edit.actor_uid;item->candidate.runtime_generation=0;item->candidate.scene_backed=true;item->changed=true;
            Text(item->candidate.actor_name,"actor name",true);
            std::unordered_set<std::string> keys;
            for(const auto &spec:item->candidate.component_specs){Require(keys.insert(spec.key).second,"duplicate component key");item->components.push_back(std::make_unique<ComponentEdit>(spec));}
            plan.order.insert(position,edit.actor_uid);plan.entries.emplace(edit.actor_uid,std::move(item));plan.structural=true;
        } else {
            auto &actor=ActorAt(plan,edit.actor_uid);actor.changed=true;
            if constexpr(std::is_same_v<T,SceneFormat::DeleteActorMutation>) {
                actor.removed=true;plan.structural=true;
                for(auto &[uid,child]:plan.entries)if(!child->removed&&child->candidate.parent_uid==edit.actor_uid){child->candidate.parent_uid=0;child->changed=true;child->pose_changed=true;}
                std::erase(plan.order,edit.actor_uid);
            } else if constexpr(std::is_same_v<T,SceneFormat::SetActorNameMutation>) {
                Text(edit.actor_name,"actor name");Require(actor.candidate.actor_name!=edit.actor_name,"actor name is unchanged");actor.candidate.actor_name=edit.actor_name;plan.structural=true;
            } else if constexpr(std::is_same_v<T,SceneFormat::SetActorParentMutation>) {
                const UID parent=edit.parent_uid.value_or(0);Require(parent!=actor.candidate.parent_uid,"actor parent is unchanged");
                if(parent)ActorAt(plan,parent);
                auto cursor=parent;std::unordered_set<UID> visited;
                while(cursor){Require(cursor!=edit.actor_uid&&visited.insert(cursor).second,"scene edit creates a parent cycle");cursor=ParentOf(ActorAt(plan,cursor));}
                const auto old_world=World(plan,edit.actor_uid);const auto parent_world=parent?World(plan,parent):Pose{};
                actor.candidate.parent_uid=parent;actor.pose_changed=true;
                if(auto *transform=Primary(actor,"Transform")) {
                    float x,y;Rotate(old_world.x-parent_world.x,old_world.y-parent_world.y,-parent_world.rotation,x,y);
                    Patch(*transform,"x",double(x));Patch(*transform,"y",double(y));Patch(*transform,"rotation",double(old_world.rotation-parent_world.rotation));
                }
            } else if constexpr(std::is_same_v<T,SceneFormat::AddComponentMutation>) {
                Require(!Find(actor,edit.component_spec.key),"component key already exists");
                actor.components.push_back(std::make_unique<ComponentEdit>(edit.component_spec));plan.structural=true;
            } else if constexpr(std::is_same_v<T,SceneFormat::DeleteComponentMutation>) {
                At(actor,edit.component_key).removed=true;plan.structural=true;
            } else if constexpr(std::is_same_v<T,SceneFormat::RenameComponentMutation>) {
                Text(edit.new_component_key,"component key");Require(!Find(actor,edit.new_component_key),"component key already exists");
                auto &component=At(actor,edit.component_key);component.spec.key=component.effective.key=edit.new_component_key;component.changed=true;plan.structural=true;
            } else if constexpr(std::is_same_v<T,SceneFormat::SetComponentTypeMutation>) {
                auto &component=At(actor,edit.component_key);Require(component.spec.type!=edit.type_name,"component type is unchanged");
                component.spec.type=edit.type_name;component.effective=component.spec;component.replace=true;component.changed=true;component.adopt=false;plan.structural=true;
            } else if constexpr(std::is_same_v<T,SceneFormat::SetComponentPropertyMutation>) {
                auto &component=At(actor,edit.component_key);Patch(component,edit.property_name,edit.value);
                if((component.spec.type=="Transform"||component.spec.type=="Rigidbody")&&(edit.property_name=="x"||edit.property_name=="y"||edit.property_name=="rotation"))actor.pose_changed=true;
                if(component.spec.type=="Rigidbody"&&(edit.property_name=="x"||edit.property_name=="y"||edit.property_name=="rotation")) {
                    if(auto *transform=Primary(actor,"Transform")) {
                        const auto parent=actor.candidate.parent_uid?World(plan,actor.candidate.parent_uid):Pose{};
                        auto desired=World(plan,edit.actor_uid);
                        if(edit.property_name=="x")desired.x=static_cast<float>(Number(component.effective,"x"));
                        else if(edit.property_name=="y")desired.y=static_cast<float>(Number(component.effective,"y"));
                        else desired.rotation=static_cast<float>(Number(component.effective,"rotation"));
                        float x,y;Rotate(desired.x-parent.x,desired.y-parent.y,-parent.rotation,x,y);
                        Patch(*transform,"x",double(x));Patch(*transform,"y",double(y));Patch(*transform,"rotation",double(desired.rotation-parent.rotation));
                    }
                }
            }
        }
    },mutation.payload);
}
std::unique_ptr<ComponentRecord> Prepare(const Spec &spec) {
    Text(spec.key,"component key");Text(spec.type,"component type");
    Ref prototype(Runtime().lua_state);
    if(!IsBuiltinComponentType(spec.type)) {
        const auto found=Runtime().component_type_tables.find(spec.type);
        Require(found!=Runtime().component_type_tables.end(),"component type is not registered: "+spec.type);prototype=found->second;
    }
    return PrepareComponentRecord(spec,prototype);
}
std::unique_ptr<ComponentRecord> View(const Spec &spec,const Ref &reference) {
    return std::make_unique<ComponentRecord>(spec.key,spec.type,reference,false,false,false,false,false,false,false,false);
}
bool PoseAffected(Plan &plan,UID uid) {
    std::vector<UID> path;std::unordered_set<UID> visited;bool result=false;
    while(uid) {
        if(const auto cached=plan.affected_cache.find(uid);cached!=plan.affected_cache.end()){result=cached->second;break;}
        Require(visited.insert(uid).second,"scene edit creates a parent cycle");
        auto &actor=ActorAt(plan,uid);path.push_back(uid);
        if(actor.pose_changed){result=true;break;}
        if(!Primary(actor,"Transform"))break;
        uid=ParentOf(actor);
    }
    for(const UID ancestor:path)plan.affected_cache.emplace(ancestor,result);
    return result;
}
void ValidateHierarchy(Plan &plan) {
    std::unordered_set<UID> complete;
    for(const auto &[uid,item]:plan.entries) {
        if(item->removed||complete.contains(uid))continue;
        std::vector<UID> path;std::unordered_set<UID> visiting;auto cursor=uid;
        while(cursor&&!complete.contains(cursor)) {
            Require(visiting.insert(cursor).second,"scene edit creates a parent cycle");
            path.push_back(cursor);cursor=ParentOf(ActorAt(plan,cursor));
        }
        complete.insert(path.begin(),path.end());
    }
}
// Pose edits update authored world-space body coordinates as well as the live
// body. Otherwise a later fixture/configuration rebuild replays stale positions.
// Keep this separate from Patch: linking pose must not replace body userdata.
void SynchronizeLinkedPoses(Plan &plan) {
    for(auto &[uid,item]:plan.entries) {
        if(item->removed||!Primary(*item,"Transform")||!PoseAffected(plan,uid))continue;
        auto *body=Primary(*item,"Rigidbody");if(!body)continue;
        const auto pose=World(plan,uid);
        for(const auto &[name,value]:std::initializer_list<std::pair<const char *,double>>{{"x",pose.x},{"y",pose.y},{"rotation",pose.rotation}}) {
            SceneFormat::UpsertComponentProperty(body->spec,name,value);
            SceneFormat::UpsertComponentProperty(body->effective,name,value);
        }
        item->changed=true;
    }
}
void PrepareFinal(Plan &plan) {
    ValidateHierarchy(plan);
    for(auto &[uid,item]:plan.entries) {
        auto &actor=*item;if(actor.removed)continue;
        if(PoseAffected(plan,uid)) {
            const auto pose=World(plan,uid);
            if(Primary(actor,"Rigidbody")) {
                Rigidbody::ValidateNumericProperty("x",pose.x);Rigidbody::ValidateNumericProperty("y",pose.y);Rigidbody::ValidateNumericProperty("rotation",pose.rotation);
            }
        }
        if(!actor.changed)continue;
        Actor effective_actor=actor.candidate;effective_actor.component_specs.clear();actor.candidate.component_specs.clear();
        std::vector<std::unique_ptr<ComponentRecord>> asset_records;
        for(auto &entry:actor.components) {
            auto &component=*entry;if(component.removed)continue;
            if(component.authored)actor.candidate.component_specs.push_back(component.spec);
            effective_actor.component_specs.push_back(component.effective);
            const bool fresh=component.original.isNil()||component.replace;
            if(fresh||component.adopt||(component.changed&&IsBuiltinComponentType(component.spec.type))) {
                // Replacements retain the established authored-config semantics;
                // in-place candidates include live fields so unrelated state is preserved.
                component.prepared=Prepare(fresh?component.spec:component.effective);
            } else if(component.changed) {
                Require(component.spec.key.size()<=256,"component key is too long");
            }
            if(component.prepared&&(fresh||component.adopt))asset_records.push_back(View(component.effective,component.prepared->instance_table));
            else if(component.spec.type!="Tilemap"&&component.spec.type!="SpriteAnimation") {
                if(component.prepared)asset_records.push_back(View(component.effective,component.prepared->instance_table));
                else asset_records.push_back(View(component.effective,component.original));
            }
        }
        SceneFormat::SortComponentSpecs(actor.candidate.component_specs);
        PrepareDetachedActorAssets(effective_actor,asset_records);
        const auto world=World(plan,uid);
        if(Primary(actor,"Rigidbody")) {
            Rigidbody::ValidateNumericProperty("x",world.x);Rigidbody::ValidateNumericProperty("y",world.y);Rigidbody::ValidateNumericProperty("rotation",world.rotation);
        }
        if(auto *map=Primary(actor,"Tilemap")) {
            auto *transform=Primary(actor,"Transform");const auto local=transform?Local(*transform):Pose{};
            Require(local.rotation==0,"Tilemap rotation must be zero");
            if(!map->prepared&& !map->original.isNil()) {
                auto *native=LuaBoundary::Cast<Tilemap *>(map->original);
                if(native->HasAsset())Tilemap::ValidateInitialization(native->GetAsset(),local.x,local.y);
            }
        }
        for(auto &component:actor.components)if(!component->removed&&component->spec.type=="Transform"&&component->prepared) {
            auto *transform=LuaBoundary::Cast<Transform *>(component->prepared->instance_table);
            transform->world_x=world.x;transform->world_y=world.y;transform->world_rotation=world.rotation;
        }
        if(!actor.original)plan.nodes.push_back(actor.candidate);
    }
}
// Reindex only native bookkeeping. Existing Lua identities and pending starts
// stay intact, and deferred spawns remain deferred across an editor transaction.
void Reindex(std::list<Actor> &actors) {
    const std::unordered_set<UID> deferred(Runtime().pending_actor_uids_to_activate.begin(),Runtime().pending_actor_uids_to_activate.end());
    Runtime().actor_by_uid.clear();Runtime().actors_by_name.clear();Runtime().actor_order_by_uid.clear();Runtime().actor_uids_sorted.clear();
    Runtime().on_update_components.clear();Runtime().on_late_update_components.clear();Runtime().next_actor_order=0;
    for(auto &actor:actors)if(!actor.runtime_destroyed&&!Runtime().pending_destroy_actor_uids.contains(actor.uid)) {
        Runtime().actor_by_uid[actor.uid]=&actor;Runtime().actors_by_name[actor.actor_name].push_back(&actor);
        Runtime().actor_order_by_uid[actor.uid]=Runtime().next_actor_order++;
        SortComponentsForActor(actor.uid);
        if(deferred.contains(actor.uid))continue;
        Runtime().actor_uids_sorted.push_back(actor.uid);
        const auto found=Runtime().actor_components.find(actor.uid);if(found==Runtime().actor_components.end())continue;
        for(const auto &record:found->second)if(!record->removed) {
            if(record->has_on_update)Runtime().on_update_components.emplace_back(actor.uid,record.get());
            if(record->has_on_late_update)Runtime().on_late_update_components.emplace_back(actor.uid,record.get());
        }
    }
    Runtime().dirty_component_actor_uids.clear();
}
} // namespace

bool Engine::ApplySceneEditCommand(const SceneFormat::SceneEditCommand &command) {
    WorldScope world_scope(world_);
    if(command.empty())return false;
    last_scene_edit_error_.clear();
    if(scene_reset_required_||scene_transition_active_) {last_scene_edit_error_="runtime cannot edit during scene transition or before reset";return false;}
    bool committing=false;
    struct Transition {bool &flag;explicit Transition(bool &value):flag(value){flag=true;}~Transition(){flag=false;}} transition(scene_transition_active_);
    try {
        Require(Runtime().lua_state&&Runtime().scene_actors==&actors,"scene edit requires a bound runtime");
        Require(!RayCast::GetPhysicsWorld()||!RayCast::GetPhysicsWorld()->IsLocked(),"scene edits cannot run during a physics callback");
        Plan plan;BuildSnapshot(plan,actors);
        for(const auto &mutation:command.mutations) {
            plan.world_cache.clear();plan.affected_cache.clear();
            Simulate(plan,mutation);
            plan.world_cache.clear();plan.affected_cache.clear();
            SynchronizeLinkedPoses(plan);
        }
        PrepareFinal(plan);
        std::vector<EventBus::ComponentKeyRemap> event_keys;
        for(const auto &[uid,item]:plan.entries)if(!item->removed)for(const auto &component:item->components)
            if(!component->removed&&!component->replace&&!component->original.isNil()&&component->original_key!=component->spec.key)
                event_keys.push_back({component->original,uid,component->original_key,component->spec.key});
        if(!event_keys.empty()) {
            std::string error;Require(EventBus::PrepareKeyRemap(event_keys,plan.event_keys,error),error);
        }
        const auto errors_before=ComponentManager::GetRuntimeErrorCount();
        committing=true;
        // Stop affected timelines while their original renderer keys still resolve.
        for(auto &[uid,item]:plan.entries)if(item->changed)for(auto &entry:item->components) {
            auto &component=*entry;
            if(component.original_type=="SpriteAnimation"&&!component.original.isNil()&&(item->removed||component.removed||component.replace||component.adopt))
                LuaBoundary::Cast<SpriteAnimation *>(component.original)->Stop();
        }
        bool teardown=false;
        for(auto &[uid,item]:plan.entries) {
            if(item->removed&&item->original){ComponentManager::DestroyActor(item->original);teardown=true;continue;}
            if(item->removed)continue;
            for(auto &entry:item->components)if(!entry->original.isNil()&&(entry->removed||entry->replace)) {
                ComponentManager::RemoveComponent(uid,entry->original);teardown=true;
            }
        }
        if(teardown)ComponentManager::FinalizePrePhysicsDestructions();
        Require(ComponentManager::GetRuntimeErrorCount()==errors_before,"scene edit teardown callback failed: "+ComponentManager::GetLastRuntimeError());
        for(auto &[uid,item]:plan.entries)if(!item->removed&&item->original) {
            const auto found=Runtime().actor_by_uid.find(uid);
            Require(found!=Runtime().actor_by_uid.end()&&found->second==item->original&&!item->original->runtime_destroyed&&item->original->runtime_generation==item->candidate.runtime_generation,"scene edit owner changed during commit");
            for(auto &entry:item->components)if(!entry->removed&&!entry->replace&&!entry->original.isNil()) {
                auto *record=FindComponentRecord(uid,entry->original_key);
                Require(record&&AreSameLuaRef(record->instance_table,entry->original),"scene edit component changed during teardown");
                entry->original_record=record;
            }
        }
        // Publish preallocated nodes in final document order; splice preserves
        // every old Actor address, including unrelated actors created by callbacks.
        for(auto &node:plan.nodes){node.AssignRuntimeGeneration();Runtime().actor_by_uid[node.uid]=&node;NotifyRuntimeActorPublished(node);}
        actors.splice(actors.end(),plan.nodes);
        if(plan.structural) {
            std::unordered_map<UID,std::list<Actor>::iterator> positions;
            for(auto position=actors.begin();position!=actors.end();++position)if(!position->runtime_destroyed)positions.emplace(position->uid,position);
            std::list<Actor> ordered;
            for(const UID uid:plan.order) {
                const auto position=positions.find(uid);Require(position!=positions.end(),"prepared actor disappeared during commit");
                ordered.splice(ordered.end(),actors,position->second);
            }
            // Callback-created actors keep their relative order after the
            // prepared scene, rather than jumping ahead of every old actor.
            ordered.splice(ordered.end(),actors);
            actors.splice(actors.end(),ordered);
        }
        for(auto &[uid,item]:plan.entries)if(!item->removed&&item->changed) {
            auto *live=Runtime().actor_by_uid.at(uid);
            live->actor_name.swap(item->candidate.actor_name);live->parent_uid=item->candidate.parent_uid;live->component_specs.swap(item->candidate.component_specs);
            // Rename as a simultaneous key update, allowing a->temp,b->a,temp->b.
            for(auto &entry:item->components)if(!entry->removed&&!entry->replace&&!entry->original.isNil()&&entry->original_key!=entry->spec.key) {
                auto *record=entry->original_record;
                Require(record&&!record->removed&&AreSameLuaRef(record->instance_table,entry->original),"renamed component changed during commit");
                SetSystemField(record->instance_table,"key",Ref(Runtime().lua_state,entry->spec.key));record->key=entry->spec.key;

            }
            for(auto &pending:Runtime().pending_on_start)if(pending.actor_uid==uid) {
                for(const auto &entry:item->components)if(!entry->removed&&!entry->replace&&entry->original_key==pending.component_key) { pending.component_key=entry->spec.key;break; }
            }
            for(auto &entry:item->components)if(!entry->removed&&(entry->original.isNil()||entry->replace)) {
                Require(!FindComponentRecord(uid,entry->spec.key),"component key was occupied by a commit callback");
                InstallPreparedComponent(uid,std::move(entry->prepared));
            }
            SortComponentsForActor(uid);
        }
        EventBus::CommitKeyRemap(std::move(plan.event_keys));
        if(plan.structural||teardown)Reindex(actors);
        for(auto &[uid,item]:plan.entries)if(!item->removed&&item->changed) {
            for(auto &entry:item->components) {
                auto &component=*entry;if(component.removed||component.original.isNil()||component.replace)continue;
                Require(IsComponentRefAlive(component.original,uid,component.spec.key),"edited component changed during commit");
                if(component.adopt) {
                    if(component.spec.type=="Tilemap")LuaBoundary::Cast<Tilemap *>(component.original)->AdoptPreparedState(*LuaBoundary::Cast<Tilemap *>(component.prepared->instance_table));
                    else LuaBoundary::Cast<SpriteAnimation *>(component.original)->AdoptPreparedState(*LuaBoundary::Cast<SpriteAnimation *>(component.prepared->instance_table));
                } else for(const auto &[name,value]:component.patches) {
                    if(component.spec.type=="Transform")AssignPropertyValueToLuaField(component.original,name,value);
                    else Require(ComponentManager::SetComponentPropertyValue(uid,component.spec.key,name,value),"prepared property commit failed: "+component.spec.key+"."+name);
                }
            }
            ApplyDetachedActorPoses(uid);
        }
        ComponentManager::ResolveTransformHierarchy();
        for(auto &[uid,item]:plan.entries)if(!item->removed&&PoseAffected(plan,uid)) {
            auto *body=FindPrimaryComponentByType(uid,"Rigidbody");
            auto *transform=FindPrimaryComponentByType(uid,"Transform");
            if(body&&transform) {
                const auto *pose=LuaBoundary::Cast<Transform *>(transform->instance_table);auto *native=LuaBoundary::Cast<Rigidbody *>(body->instance_table);
                native->SetPosition(b2Vec2(pose->world_x,pose->world_y));native->SetRotation(pose->world_rotation);
            }
        }
        Require(ComponentManager::GetRuntimeErrorCount()==errors_before,"scene edit commit callback failed: "+ComponentManager::GetLastRuntimeError());
        for(auto &[uid,item]:plan.entries)if(!item->removed&&item->changed) {
            const auto owner=Runtime().actor_by_uid.find(uid);
            Require(owner!=Runtime().actor_by_uid.end()&&!owner->second->runtime_destroyed&&!Runtime().pending_destroy_actor_uids.contains(uid),"scene edit owner was destroyed by a commit callback");
            for(const auto &entry:item->components)if(!entry->removed) {
                const auto *record=FindComponentRecord(uid,entry->spec.key);
                Require(record&&record->type==entry->spec.type,"scene edit component was removed by a commit callback");
                if(!entry->replace&&!entry->original.isNil())Require(AreSameLuaRef(record->instance_table,entry->original),"scene edit identity changed during commit callback");
            }
        }
        last_scene_edit_error_.clear();
        actors.remove_if([](const Actor &actor){return actor.runtime_destroyed;});
        rebuildRuntimeActorUIDMap();invalidateRuntimePhysicsHierarchyCache();
        return true;
    } catch(const std::exception &failure) {
        const auto message=ScriptReloadCandidate::SanitizeDiagnosticText(failure.what());
        if(committing) { failSceneCommit("scene edit commit failed: "+message); last_scene_edit_error_=message; }
        else last_scene_edit_error_=message;
        return false;
    }
}
