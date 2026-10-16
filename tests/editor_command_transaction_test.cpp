#include "core/Engine.h"
#include "core/FrameClock.h"
#include "editor/core/EditorSceneSession.h"
#include "physics/Rigidbody.h"
#include "scene/ActorHandle.h"
#include "scripting/ComponentManager.h"
#include "shared/resources/ResourcePath.h"
#include "shared/scene_format/SceneMutation.h"
#include "../src/engine/scripting/manager/Internal.h"
#include "LuaBridge/LuaBridge.h"
#include "lua.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <tuple>

namespace {
namespace fs = std::filesystem;
using namespace ManagerDetail;
void Check(bool value, const std::string &message) { if (!value) throw std::runtime_error(message); }
void Put(const fs::path &path, const std::string &text) {
    fs::create_directories(path.parent_path()); std::ofstream stream(path); stream << text;
    Check(stream.good(), "cannot write test fixture");
}
struct Fixture {
    fs::path previous = ResourcePath::ResourcesRootPath();
    fs::path root = fs::temp_directory_path() / ("pocket-editor-command-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixture() {
        Put(root / "game.config", R"({"initial_scene":"main"})");
        Put(root / "scenes/main.scene", R"({"actors":[{"uid":1,"name":"source","components":{"good":{"type":"Good"},"body":{"type":"Rigidbody","body_type":"static","has_trigger":false},"transform":{"type":"Transform"}}}]})");
        Put(root / "component_types/Good.lua", R"(
AUDIT={starts=0,destroys=0}
Good={health=100,updates=0,state={value=7},
 OnStart=function(self)
  AUDIT.starts=AUDIT.starts+1
  if self.actor:GetUID()~=1 then
   local source=Actor.Find('source')
   if source then local good=source:GetComponent('Good');good.health=good.health-10 end
  end
 end,
 OnUpdate=function(self) self.updates=self.updates+1 end,
 OnDestroy=function(self) AUDIT.destroys=AUDIT.destroys+1 end}
)");
        Put(root / "component_types/Bad.lua", "Bad={owned=Rigidbody()}");
        ResourcePath::SetResourcesRootPath(root);
    }
    ~Fixture() { Actor::ClearTemplateCache(); ResourcePath::SetResourcesRootPath(previous);
        std::error_code error; fs::remove_all(root, error); }
};
void Lua(const std::string &code) {
    auto *state = Runtime().lua_state;
    const int top = lua_gettop(state);
    if (luaL_dostring(state, code.c_str()) != LUA_OK) {
        std::string error = lua_tostring(state, -1); lua_settop(state, top); throw std::runtime_error(error);
    }
    lua_settop(state, top);
}
auto Queues(const Engine &engine) {
    std::size_t records = 0;
    for (const auto &[uid, values] : Runtime().actor_components) records += values.size();
    return std::tuple{engine.GetRuntimeActors().size(), records,
        Runtime().actor_by_uid.size(), Runtime().actor_components.size(),
        Runtime().pending_on_start.size(), Runtime().pending_actor_uids_to_activate.size(),
        Runtime().pending_destroy_actor_uids.size(), Runtime().dirty_component_actor_uids.size()};
}
struct Identity {
    luabridge::LuaRef good = ComponentManager::GetComponentByKey(1, "good");
    luabridge::LuaRef nested = good["state"];
    luabridge::LuaRef body = ComponentManager::GetComponentByKey(1, "body");
    ActorHandle actor{Runtime().actor_by_uid.at(1)};
    b2Body *native_body = body.cast<Rigidbody *>().value()->body;
    void AssertRetained(const Engine &engine) const {
        Check(actor.IsValid() && actor.Resolve() == engine.GetRuntimeActorByUID(1), "original Actor identity changed");
        auto current = ComponentManager::GetComponentByKey(1, "good");
        Check(good.rawequal(current) && nested.rawequal(current["state"]), "original Lua instance/nested state changed");
        auto current_body = ComponentManager::GetComponentByKey(1, "body");
        Check(body.rawequal(current_body) && native_body == current_body.cast<Rigidbody *>().value()->body,
              "original native userdata or Box2D body changed");
    }
};
SceneFormat::SetActorNameMutation Rename(const std::string &name) { return {1, name}; }
SceneFormat::AddComponentMutation BadAdd() {
    SceneFormat::AddComponentMutation mutation;
    mutation.actor_uid = 1; mutation.component_spec.key = "bad"; mutation.component_spec.type = "Bad";
    return mutation;
}
SceneFormat::CreateActorMutation BadCreate() {
    SceneFormat::CreateActorMutation mutation;
    mutation.actor_uid = 2; mutation.actor_record.uid = 2; mutation.actor_record.name = "partial";
    mutation.actor_record.component_specs = {{"a_good", "Good", {}}, {"z_bad", "Bad", {}}};
    return mutation;
}
void TestHostAtomicCommands() {
    Fixture fixture; Engine engine; engine.InitializeHeadless(); engine.StepSimulation();
    Identity identity;
    const auto original_queues = Queues(engine);
    const auto original_specs = engine.GetRuntimeActorByUID(1)->component_specs.size();
    for (const auto &command : {
             SceneFormat::SceneEditCommand::Single({BadAdd()}),
             SceneFormat::SceneEditCommand::Single({BadCreate()}),
             SceneFormat::SceneEditCommand{{{Rename("should_not_commit")}, {BadAdd()}}},
             SceneFormat::SceneEditCommand{{{Rename("also_rejected")}, {SceneFormat::DeleteActorMutation{999}}}}}) {
        const auto frame = FrameClock::GetFrameNumber();
        Check(!engine.ApplySceneEditCommand(command), "invalid host edit accepted");
        Check(FrameClock::GetFrameNumber() == frame, "host edit advanced simulation");
        Check(Queues(engine) == original_queues, "rejected host command changed component/actor queues");
        Check(engine.GetRuntimeActorByUID(1)->component_specs.size() == original_specs,
              "rejected AddComponent left a ghost authoring specification");
        Check(engine.GetRuntimeActorByUID(1)->actor_name == "source", "rejected batch retained earlier rename");
        Check(ComponentManager::FindActorByName("partial").isNil(), "rejected CreateActor remained findable");
        Check(ComponentManager::GetComponentByKey(1, "bad").isNil(), "rejected component remained live");
        identity.AssertRetained(engine);
        Lua("assert(AUDIT.starts==1 and AUDIT.destroys==0); assert(Actor.Find('source'):GetComponent('Good').health==100)");
        engine.StepSimulation();
        Check(Queues(engine) == original_queues, "rejected command left delayed lifecycle work");
        identity.AssertRetained(engine);
        Lua("assert(AUDIT.starts==1 and AUDIT.destroys==0); assert(Actor.Find('source'):GetComponent('Good').health==100)");
        Check(!engine.RequiresSceneReset(), "validation rejection requires unnecessary runtime reset");
    }
    SceneFormat::SetComponentPropertyMutation bad_width{1, "body", "width", 0.0};
    const float width = identity.body.cast<Rigidbody *>().value()->width;
    Check(!engine.ApplySceneEditCommand(SceneFormat::SceneEditCommand{{{Rename("bad_width")}, {bad_width}}}),
          "invalid native property batch accepted");
    Check(identity.body.cast<Rigidbody *>().value()->width == width && engine.GetRuntimeActorByUID(1)->actor_name == "source",
          "invalid native property batch partially changed configuration/name");
    identity.AssertRetained(engine);
    const int starts = luabridge::getGlobal(Runtime().lua_state, "AUDIT")["starts"].cast<int>().value();
    Check(engine.ApplySceneEditCommand(SceneFormat::SceneEditCommand::Single({Rename("accepted")})),
          "valid command rejected after failed transactions");
    identity.AssertRetained(engine);
    Check(!ComponentManager::FindActorByName("accepted").isNil() && ComponentManager::FindActorByName("source").isNil(),
          "valid in-place rename did not update name index");
    Check(luabridge::getGlobal(Runtime().lua_state, "AUDIT")["starts"].cast<int>().value() == starts,
          "valid rename restarted lifecycle");
    Check(ComponentManager::GetRuntimeErrorCount() == 0, "handled host rejection poisoned runtime");
}
void TestStandaloneDocumentBatch() {
    Fixture fixture; SceneDocument document;
    Check(document.LoadFromSceneName("main"), "load standalone document");
    const auto failed = SceneFormat::SceneEditCommand{{{Rename("partial")}, {SceneFormat::DeleteActorMutation{999}}}};
    Check(!document.ApplyEditCommand(failed), "structurally invalid document command accepted");
    Check(document.GetActorRecords()[0].name == "source" && !document.IsDirty(),
          "failed document batch changed clean document");
    Check(document.SetActorName(0, "existing_dirty_draft"), "prepare unrelated dirty draft");
    Check(!document.ApplyEditCommand(failed), "invalid command accepted on dirty document");
    Check(document.GetActorRecords()[0].name == "existing_dirty_draft" && document.IsDirty(),
          "failed new edit discarded/replaced prior dirty draft");
}

void TestSessionCoordinator() {
    Fixture fixture; Engine engine; engine.InitializeHeadless();
    EditorSceneSession session; session.LoadInitialScene(engine);
    session.SyncRuntimeMirrorIfDirty(engine); engine.StepSimulation();
    Check(!session.IsRuntimeMirrorPending(), "initial valid document did not synchronize");
    auto &document = session.GetSceneDocument();
    Identity identity;
    const auto frame = FrameClock::GetFrameNumber();
    SceneFormat::SceneEditCommand rename_receipt;
    Check(document.SetActorName(0, "existing_dirty_draft", &rename_receipt), "bound document rename failed");
    Check(engine.GetRuntimeActorByUID(1)->actor_name == "existing_dirty_draft" && document.IsDirty(),
          "bound document did not commit runtime before returning success");
    Check(session.HandleSceneEditCommand(rename_receipt, engine), "committed rename receipt was not acknowledged");
    identity.AssertRetained(engine);
    Check(FrameClock::GetFrameNumber() == frame, "document edit/receipt advanced gameplay");
    const auto before_queues = Queues(engine);
    const auto before_specs = document.GetActorRecords()[0].component_specs.size();
    Check(!document.AddComponentToActor(0, "Bad"), "bad default accepted into synced document");
    Check(!session.GetLastEditError().empty() && !session.IsRuntimeMirrorPending(),
          "rejected new edit must retain a usable synchronized baseline and diagnostic");
    session.ClearEditError();
    Check(session.GetLastEditError().empty() && !session.IsRuntimeMirrorPending(),
          "dismiss must clear a non-pending edit error without changing mirror state");
    Check(document.IsDirty() && document.GetActorRecords()[0].name == "existing_dirty_draft" &&
          document.GetActorRecords()[0].component_specs.size() == before_specs,
          "failed new component damaged the previous dirty draft");
    Check(Queues(engine) == before_queues, "failed bound edit left runtime component/lifecycle work");
    Check(!document.ApplyEditCommand(SceneFormat::SceneEditCommand::Single({BadCreate()})),
          "bound document accepted a partially preparable actor");
    Check(document.GetActorCount() == 1 && ComponentManager::FindActorByName("partial").isNil(),
          "failed bound CreateActor changed document or runtime");
    Check(!document.SetComponentProperty(0, "body", "width", 0.0), "bound invalid native value accepted");
    Check(Queues(engine) == before_queues, "invalid native edit changed lifecycle queues");
    identity.AssertRetained(engine);
    auto audit = luabridge::getGlobal(Runtime().lua_state, "AUDIT");
    const int starts = audit["starts"].cast<int>().value(), destroys = audit["destroys"].cast<int>().value();
    engine.StepSimulation();
    Check(audit["starts"].cast<int>().value() == starts && audit["destroys"].cast<int>().value() == destroys,
          "rejected document command scheduled gameplay callbacks");
    Check(identity.good["health"].cast<int>().value() == 100, "rejected candidate damaged existing gameplay");

    // The caller's command object and copies are receipts after the bound
    // document commits. Acknowledging them must not publish the actor twice.
    SceneFormat::CreateActorMutation valid_create;
    valid_create.actor_uid = 2; valid_create.actor_record.uid = 2; valid_create.actor_record.name = "accepted_actor";
    valid_create.actor_record.component_specs = {{"good", "Good", {}}};
    auto create_receipt = SceneFormat::SceneEditCommand::Single({valid_create});
    Check(document.ApplyEditCommand(create_receipt), "valid bound CreateActor failed");
    auto copied_receipt = create_receipt;
    const auto after_create = Queues(engine);
    Check(session.HandleSceneEditCommand(create_receipt, engine) &&
          session.HandleSceneEditCommand(copied_receipt, engine), "committed create receipt copy was replayed/rejected");
    Check(Queues(engine) == after_create && document.GetActorCount() == 2,
          "acknowledgment duplicated or reinstalled an actor");
    identity.AssertRetained(engine);
    engine.StepSimulation();
    Check(audit["starts"].cast<int>().value() == starts + 1, "accepted actor did not start exactly once");

    auto raw = SceneFormat::SceneEditCommand::Single({Rename("raw_command")});
    Check(session.HandleSceneEditCommand(raw, engine), "raw programmatic command rejected");
    Check(document.GetActorRecords()[0].name == "raw_command" && engine.GetRuntimeActorByUID(1)->actor_name == "raw_command",
          "raw command must update both active document and runtime");
    identity.AssertRetained(engine);
    SceneDocument detached = document;
    Check(detached.SetActorName(0, "detached_copy"), "detached document cannot edit");
    Check(document.GetActorRecords()[0].name == "raw_command" && engine.GetRuntimeActorByUID(1)->actor_name == "raw_command",
          "copied document retained a live coordinator hook");

    // Repair the rejected prototype, retry the actual UI-facing operation,
    // then remove that component so later play snapshots stay valid.
    Lua("Bad.owned=nil");
    Check(document.AddComponentToActor(0, "Bad"), "fixed prototype could not be retried");
    std::string key;
    for (const auto &spec : document.GetActorRecords()[0].component_specs) if (spec.type == "Bad") key = spec.key;
    Check(!key.empty() && !ComponentManager::GetComponentByKey(1, key).isNil(), "retry did not publish repaired component");
    Check(document.DeleteComponent(0, key), "remove repaired temporary component");
    Lua("Bad.owned=Rigidbody()");
    const std::string authoring_name = document.GetActorRecords()[0].name;
    Check(document.IsDirty() && session.EnterPlayMode(engine), "enter play with existing unsaved authoring draft");
    engine.StepSimulation();
    Check(session.TogglePlayPause() && session.IsPlayModePaused(), "pause play snapshot");
    const auto paused_frame = FrameClock::GetFrameNumber();
    Identity play_identity;
    Check(play_identity.native_body != nullptr, "paused play must retain an actual initialized Box2D body");
    auto &play_document = session.GetSceneDocument();
    SceneFormat::SceneEditCommand play_receipt;
    Check(play_document.SetActorName(0, "paused_play_edit", &play_receipt), "paused play document edit rejected");
    Check(!play_document.AddComponentToActor(0, "Bad"), "paused play accepted bad default");
    Check(play_document.GetActorRecords()[0].name == "paused_play_edit" &&
          engine.GetRuntimeActorByUID(1)->actor_name == "paused_play_edit", "failed paused edit reverted successful play draft");
    Check(session.GetAuthoringSceneDocument().GetActorRecords()[0].name == authoring_name &&
          session.GetAuthoringSceneDocument().IsDirty(), "play editing replaced/cleared unsaved authoring draft");
    Check(session.IsPlayModeActive() && session.IsPlayModePaused() && FrameClock::GetFrameNumber() == paused_frame,
          "edit changed play/pause state or advanced frame");
    play_identity.AssertRetained(engine);
    const auto play_queues = Queues(engine);
    Check(!session.HandleSceneEditCommand(rename_receipt, engine),
          "receipt from before EnterPlayMode was replayed into the new play world");
    auto fresh_but_blocked = SceneFormat::SceneEditCommand::Single({Rename("must_not_apply")});
    Check(!session.HandleSceneEditCommands({fresh_but_blocked, create_receipt}, engine),
          "batch containing an expired receipt was accepted");
    Check(play_document.GetActorRecords()[0].name == "paused_play_edit" &&
          engine.GetRuntimeActorByUID(1)->actor_name == "paused_play_edit" && Queues(engine) == play_queues &&
          FrameClock::GetFrameNumber() == paused_frame && !session.GetLastEditError().empty(),
          "stale single/batch delivery partially changed the new play world");
    play_identity.AssertRetained(engine);
    // Explicit authoring access during play is draft-only. It must remain
    // repairable without leaking the edits into the live play world.
    auto &authoring = session.GetAuthoringSceneDocument();
    const bool active_pending = session.IsRuntimeMirrorPending();
    Check(authoring.SetActorName(0, "offline_authoring_edit") && authoring.AddComponentToActor(0, "Bad"),
          "inactive authoring document must accept draft-only edits during play");
    Check(engine.GetRuntimeActorByUID(1)->actor_name == "paused_play_edit" &&
          play_document.GetActorRecords()[0].name == "paused_play_edit" &&
          session.IsRuntimeMirrorPending() == active_pending,
          "inactive authoring edit leaked into active play world or pending state");
    Check(!session.ExitPlayMode(engine), "invalid authoring snapshot accepted on play exit");
    Check(session.IsPlayModeActive() && session.IsPlayModePaused() && FrameClock::GetFrameNumber() == paused_frame &&
          authoring.GetActorRecords()[0].name == "offline_authoring_edit" && authoring.IsDirty() &&
          play_document.GetActorRecords()[0].name == "paused_play_edit",
          "failed exit discarded either draft or changed paused play state");
    play_identity.AssertRetained(engine);
    std::string authoring_bad_key;
    for (const auto &spec : authoring.GetActorRecords()[0].component_specs)
        if (spec.type == "Bad") authoring_bad_key = spec.key;
    Check(!authoring_bad_key.empty() && authoring.DeleteComponent(0, authoring_bad_key) &&
          authoring.SetActorName(0, authoring_name), "cannot repair inactive authoring draft");
    Check(session.ExitPlayMode(engine), "cannot restore authoring snapshot after rejected play edit");
    Check(!session.IsPlayModeActive() && session.GetSceneDocument().GetActorRecords()[0].name == authoring_name &&
          engine.GetRuntimeActorByUID(1)->actor_name == authoring_name && session.GetSceneDocument().IsDirty(),
          "leaving play discarded or persisted the wrong draft");
    const ActorHandle restored(engine.GetRuntimeActorByUID(1));
    const auto restored_queues = Queues(engine);
    Check(!session.HandleSceneEditCommand(play_receipt, engine),
          "late play receipt was replayed after ExitPlayMode");
    Check(restored.IsValid() && session.GetSceneDocument().GetActorRecords()[0].name == authoring_name &&
          engine.GetRuntimeActorByUID(1)->actor_name == authoring_name && Queues(engine) == restored_queues &&
          FrameClock::GetFrameNumber() == paused_frame,
          "expired play receipt mutated restored authoring/runtime state");
}
void TestPendingDraftRepair() {
    Fixture fixture; Engine engine; engine.InitializeHeadless(); engine.StepSimulation();
    Identity identity;
    // A separately edited file can be invalid even while the previously
    // loaded runtime is healthy. Load it into the editor without replacing
    // that running world, then repair multiple errors one at a time.
    Put(fixture.root / "scenes/main.scene", R"({"actors":[{"uid":1,"name":"invalid_disk_draft","components":{"good":{"type":"Good"},"body":{"type":"Rigidbody","body_type":"static","has_trigger":false},"bad_a":{"type":"Bad"},"bad_b":{"type":"Bad"}}}]})");
    EditorSceneSession session; session.LoadInitialScene(engine);
    Check(session.IsRuntimeMirrorPending(), "unmirrored initial document must be pending");
    auto &document = session.GetSceneDocument();
    Check(document.SetActorName(0, "kept_repair_draft"), "pending document must allow structurally valid repair edits");
    auto attempt = engine.GetSceneLoadAttemptRevision();
    session.SyncRuntimeMirrorIfDirty(engine);
    Check(engine.GetSceneLoadAttemptRevision() == attempt + 1 && session.IsRuntimeMirrorPending() &&
          !session.GetLastEditError().empty(), "failed mirror must preserve pending state and error");
    const auto pending_error = session.GetLastEditError();
    session.ClearEditError();
    Check(session.IsRuntimeMirrorPending() && session.GetLastEditError() == pending_error,
          "dismiss cleared an unresolved pending mirror/error");
    identity.AssertRetained(engine);
    Check(engine.GetRuntimeActorByUID(1)->actor_name == "source" && document.GetActorRecords()[0].name == "kept_repair_draft" && document.IsDirty(),
          "failed synchronization changed old world or discarded authoring repair draft");
    attempt = engine.GetSceneLoadAttemptRevision();
    session.SyncRuntimeMirrorIfDirty(engine);
    Check(engine.GetSceneLoadAttemptRevision() == attempt, "identical invalid draft was retried every frame");
    session.RetryRuntimeMirror(); session.SyncRuntimeMirrorIfDirty(engine);
    Check(engine.GetSceneLoadAttemptRevision() == attempt + 1 && session.IsRuntimeMirrorPending(),
          "explicit mirror retry did not make a new preparation attempt");
    Check(document.DeleteComponent(0, "bad_a"), "first repair step rejected because another error remains");
    attempt = engine.GetSceneLoadAttemptRevision(); session.SyncRuntimeMirrorIfDirty(engine);
    Check(engine.GetSceneLoadAttemptRevision() == attempt + 1 && session.IsRuntimeMirrorPending(),
          "new but still-invalid draft must get one new mirror attempt");
    identity.AssertRetained(engine);
    Check(document.GetActorRecords()[0].name == "kept_repair_draft" && document.IsDirty(),
          "partial repair discarded unrelated user edits");
    Check(document.DeleteComponent(0, "bad_b"), "final repair rejected");
    session.SyncRuntimeMirrorIfDirty(engine);
    Check(!session.IsRuntimeMirrorPending() && session.GetLastEditError().empty(), "valid repaired draft did not synchronize");
    Check(engine.GetRuntimeActorByUID(1)->actor_name == "kept_repair_draft" && document.IsDirty(),
          "successful repair lost draft name or incorrectly saved/cleared dirty state");
    Check(!identity.actor.IsValid(), "full pending mirror replacement must invalidate the old actor generation");
    Check(!engine.RequiresSceneReset(), "ordinary repair workflow poisoned runtime");
}
}
int main() {
    try { TestHostAtomicCommands(); TestStandaloneDocumentBatch(); TestSessionCoordinator(); TestPendingDraftRepair();
        std::cout << "editor_command_transaction_test passed\n"; return 0; }
    catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
