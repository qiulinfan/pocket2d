#include "editor/documents/SceneDocument.h"
#include "scripting/ComponentManager.h"
#include "shared/resources/ResourcePath.h"
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
using namespace SceneFormat;
void Check(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
struct Fixture {
    std::filesystem::path previous = ResourcePath::ResourcesRootPath();
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("pocket-document-transaction-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixture() {
        std::filesystem::create_directories(root / "scenes");
        std::filesystem::create_directories(root / "actor_templates");
        ResourcePath::SetResourcesRootPath(root);
        std::ofstream(root / "scenes/test.scene") << R"({"actors":[
            {"uid":1,"name":"one","components":{"transform":{"type":"Transform","x":10}}},
            {"uid":2,"name":"two","components":{"transform":{"type":"Transform","x":20}}}]})";
        std::ofstream(root / "actor_templates/test.template") << R"({"name":"template","components":{"transform":{"type":"Transform"}}})";
        ComponentManager::Initialize();
    }
    ~Fixture() { ComponentManager::Shutdown(); ResourcePath::SetResourcesRootPath(previous); std::error_code error; std::filesystem::remove_all(root, error); }
    SceneDocument Document() const { SceneDocument document; Check(document.LoadFromSceneName("test"), "load document"); return document; }
};
bool Equal(const SceneDocument &a, const SceneDocument &b) {
    if (a.IsDirty() != b.IsDirty() || a.GetSceneName() != b.GetSceneName() || a.GetScenePath() != b.GetScenePath() ||
        a.GetSceneSubdirectory() != b.GetSceneSubdirectory() || a.GetActorCount() != b.GetActorCount()) return false;
    for (std::size_t i = 0; i < a.GetActorCount(); ++i) {
        const auto &x = a.GetActorRecords()[i]; const auto &y = b.GetActorRecords()[i];
        if (a.GetActorUID(i) != b.GetActorUID(i) || x.uid != y.uid || x.parent_uid != y.parent_uid ||
            x.name != y.name || x.template_name != y.template_name || x.component_specs.size() != y.component_specs.size()) return false;
        for (std::size_t j = 0; j < x.component_specs.size(); ++j) {
            const auto &u = x.component_specs[j]; const auto &v = y.component_specs[j];
            if (u.key != v.key || u.type != v.type || u.overrides.size() != v.overrides.size()) return false;
            for (std::size_t k = 0; k < u.overrides.size(); ++k)
                if (u.overrides[k].name != v.overrides[k].name || u.overrides[k].value != v.overrides[k].value) return false;
        }
    }
    return true;
}
SceneEditCommand Rename(const std::string &name) { return SceneEditCommand::Single({SetActorNameMutation{1, name}}); }
void AtomicFailures(const Fixture &fixture) {
    auto document = fixture.Document(); const auto clean = document;
    SceneEditCommand bad = Rename("partial"); bad.mutations.push_back({DeleteActorMutation{999}});
    Check(!document.ApplyEditCommand(bad) && Equal(document, clean), "failed batch partially renamed or dirtied clean document");
    Check(document.SetActorName(1, "unsaved draft"), "prepare unrelated draft"); const auto draft = document;
    Check(!document.ApplyEditCommand(bad) && Equal(document, draft), "failed batch lost unrelated unsaved work");
    Check(!document.ApplyEditCommand({}) && Equal(document, draft), "empty command changed document");
    auto noop = Rename("partial"); noop.mutations.push_back({SetActorNameMutation{2, "unsaved draft"}});
    Check(!document.ApplyEditCommand(noop) && Equal(document, draft), "no-op member did not reject complete batch");
    int calls = 0; bool saw_candidate = false;
    document.SetPrecommitHook([&](const SceneDocument &candidate, const SceneEditCommand &command) {
        ++calls;
        saw_candidate = command.mutations.size() == 1 && candidate.GetActorDisplayName(0) == "candidate" && Equal(document, draft);
        return false;
    });
    Check(!document.ApplyEditCommand(Rename("candidate")) && Equal(document, draft) && calls == 1 && saw_candidate,
          "hook did not see isolated completed candidate or rejection changed document");
    Check(!document.ApplyEditCommand(bad) && calls == 1, "invalid candidate invoked live hook");
    document.SetPrecommitHook([](const SceneDocument &, const SceneEditCommand &) -> bool { throw std::runtime_error("runtime rejected"); });
    Check(!document.SetActorName(0, "throw") && Equal(document, draft), "hook exception changed document");
    document.SetPrecommitHook([&](const SceneDocument &, const SceneEditCommand &) {
        Check(!document.SetActorName(1, "reentrant") && !document.Save() && !document.LoadFromSceneName("test"),
              "hook reentry modified live document or disk");
        return true;
    });
    Check(document.SetActorName(0, "accepted") && document.GetActorDisplayName(1) == "unsaved draft", "valid commit after rejection failed");
}
void PreparationExceptions(const Fixture &fixture) {
    auto document = fixture.Document(); std::size_t added = 0;
    Check(document.AppendActorFromTemplate("test", added), "prepare template actor");
    const auto original = document;
    std::ofstream(fixture.root / "actor_templates/test.template") << "{";
    auto command = Rename("partial before exception");
    command.mutations.push_back({SetComponentPropertyMutation{document.GetActorUID(added), "transform", "y", 5.0}});
    int calls = 0;
    document.SetPrecommitHook([&](const SceneDocument &, const SceneEditCommand &) { ++calls; return true; });
    Check(!document.ApplyEditCommand(command) && Equal(document, original) && calls == 0,
          "candidate preparation exception changed document or invoked hook");
    std::size_t output = 999;
    Check(!document.AppendActorFromTemplate("test", output) && output == 999 && Equal(document, original),
          "template load exception changed document or output");
    std::ofstream(fixture.root / "actor_templates/test.template") << R"({"name":"template","components":{"transform":{"type":"Transform"}}})";
}
void NineMutationKinds(const Fixture &fixture) {
    auto document = fixture.Document(); SceneEditCommand command;
    command.mutations.push_back({SetActorNameMutation{1, "renamed"}});
    command.mutations.push_back({SetActorParentMutation{2, 1}});
    command.mutations.push_back({AddComponentMutation{1, {"extra", "SpriteRenderer", {}}}});
    command.mutations.push_back({SetComponentPropertyMutation{1, "extra", "r", 42}});
    command.mutations.push_back({RenameComponentMutation{1, "extra", "renamed_sprite"}});
    command.mutations.push_back({SetComponentTypeMutation{1, "renamed_sprite", "ParticleSystem"}});
    command.mutations.push_back({DeleteComponentMutation{1, "renamed_sprite"}});
    ActorRecord created; created.uid = 3; created.name = "third";
    command.mutations.push_back({CreateActorMutation{3, 1, created}});
    command.mutations.push_back({DeleteActorMutation{3}});
    int calls = 0;
    document.SetPrecommitHook([&](const SceneDocument &candidate, const SceneEditCommand &receipt) {
        ++calls; Check(receipt.mutations.size() == 9 && candidate.GetActorCount() == 2, "hook saw partial batch"); return true;
    });
    Check(document.ApplyEditCommand(command) && calls == 1 && document.IsDirty(), "valid nine-kind batch failed");
    Check(document.GetActorDisplayName(0) == "renamed" && document.FindParentActorIndex(1) == 0 &&
          !document.HasRawComponent(0, "renamed_sprite"), "valid batch final state differs");
    float x, y, rotation;
    Check(document.TryGetActorWorldTransform(1, nullptr, x, y, rotation) && x == 20, "reparent no longer preserves world pose");
}
void PublicPathsAndCounters(const Fixture &fixture) {
    auto document = fixture.Document(); const auto baseline = document; auto control = document;
    int calls = 0;
    document.SetPrecommitHook([&](const SceneDocument &, const SceneEditCommand &) { ++calls; return false; });
    std::size_t index = 999; std::string component_key = "unchanged";
    SceneEditCommand output = Rename("untouched receipt");
    const auto reject = [&](bool result) { Check(!result && Equal(document, baseline), "public mutator bypassed transaction hook"); };
    reject(document.SetActorName(0, "new"));
    reject(document.SetActorParent(1, 0));
    reject(document.DeleteActor(0));
    reject(document.DuplicateActor(0, index, &output));
    reject(document.SetComponentType(0, "transform", "SpriteRenderer"));
    reject(document.RenameComponent(0, "transform", "renamed"));
    reject(document.DeleteComponent(0, "transform"));
    reject(document.DuplicateComponent(0, "transform", component_key));
    reject(document.SetComponentProperty(0, "transform", "y", 5.0));
    reject(document.AppendEmptyActor(index, &output));
    reject(document.AppendActorFromTemplate("test", index, &output));
    reject(document.AddComponentToActor(0, "SpriteRenderer"));
    reject(document.ApplyMutation({SetActorNameMutation{1, "single"}}));
    reject(document.ApplyEditCommand(Rename("command")));
    Check(calls == 14 && index == 999 && component_key == "unchanged" &&
          std::get<SetActorNameMutation>(output.mutations[0].payload).actor_name == "untouched receipt",
          "rejected edits changed output metadata or missed hook");
    document.SetPrecommitHook({});
    std::size_t actual = 0, expected = 0;
    Check(document.AppendEmptyActor(actual) && control.AppendEmptyActor(expected), "append after rejection failed");
    Check(Equal(document, control) && actual == expected, "rejected creation consumed UID/name allocator state");
}
double Property(const SceneDocument &document, SceneDocument::ActorUID uid, const char *key, const char *name) {
    const auto index = document.FindActorIndexByUID(uid); Check(index.has_value(), "pose actor missing");
    const auto actor = document.BuildEffectiveActor(*index);
    const auto *component = SceneFormat::FindComponentSpec(actor.component_specs, key); Check(component != nullptr, "pose component missing");
    for (const auto &property : component->overrides) if (property.name == name) {
        if (const auto *value = std::get_if<double>(&property.value)) return *value;
        if (const auto *value = std::get_if<int>(&property.value)) return *value;
    }
    throw std::runtime_error("pose field missing");
}
void Near(double value, double expected, const char *message) { Check(std::abs(value - expected) < 0.0001, message); }
void BodyMatchesWorld(const SceneDocument &document, SceneDocument::ActorUID uid) {
    const auto index = document.FindActorIndexByUID(uid); Check(index.has_value(), "world actor missing");
    float x = 0, y = 0, rotation = 0;
    Check(document.TryGetActorWorldTransform(*index, nullptr, x, y, rotation), "world pose missing");
    Near(Property(document, uid, "body", "x"), x, "body x is not world-authored");
    Near(Property(document, uid, "body", "y"), y, "body y is not world-authored");
    Near(Property(document, uid, "body", "rotation"), rotation, "body rotation is not world-authored");
}
void LinkedWorldPose(const Fixture &fixture) {
    std::ofstream(fixture.root / "actor_templates/posed.template") << R"({"components":{
        "transform":{"type":"Transform","x":2,"y":3,"rotation":15},
        "body":{"type":"Rigidbody","x":201,"y":202,"rotation":203,"density":2}}})";
    std::ofstream(fixture.root / "scenes/poses.scene") << R"({"actors":[
      {"uid":1,"name":"parent","components":{"transform":{"type":"Transform","x":10,"y":20,"rotation":90},"body":{"type":"Rigidbody","x":101,"y":102,"rotation":103}}},
      {"uid":2,"name":"child","parent_uid":1,"template":"posed"},
      {"uid":3,"name":"grandchild","parent_uid":2,"components":{"transform":{"type":"Transform","x":4,"y":5,"rotation":5},"body":{"type":"Rigidbody","x":301,"y":302,"rotation":303}}},
      {"uid":4,"name":"unrelated","components":{"transform":{"type":"Transform","x":-1,"y":-2,"rotation":0},"body":{"type":"Rigidbody","x":401,"y":402,"rotation":403}}}
    ]})";
    SceneDocument document; Check(document.LoadFromSceneName("poses"), "load pose document");
    Check(Property(document, 1, "body", "x") == 101 && Property(document, 2, "body", "x") == 201,
          "load normalized independent legacy authored poses");
    Check(document.SetComponentProperty(3, "body", "width", 2.0), "unrelated width edit failed");
    Check(Property(document, 4, "body", "x") == 401 && Property(document, 2, "body", "x") == 201,
          "non-pose edit normalized unrelated/template pose");
    Check(document.SetComponentProperty(0, "transform", "x", 11.0), "root local edit failed");
    for (const auto uid : {1, 2, 3}) BodyMatchesWorld(document, uid);
    Near(Property(document, 2, "body", "x"), 14, "rotated parent x did not propagate");
    Near(Property(document, 2, "body", "y"), 18, "rotated parent y did not propagate");
    Check(Property(document, 4, "body", "x") == 401 && Property(document, 2, "body", "density") == 2,
          "pose synchronization changed unrelated actor or inherited material");
    const auto *child_raw = SceneFormat::FindComponentSpec(document.GetActorRecords()[1].component_specs, "body");
    Check(child_raw && child_raw->overrides.size() == 3, "pose sync materialized unrelated template properties");
    Check(document.SetComponentProperty(1, "body", "x", 30.0), "world body x edit failed");
    Near(Property(document, 2, "transform", "x"), 2, "body world x was copied into local x");
    Near(Property(document, 2, "transform", "y"), 19, "body world x did not inverse-rotate into local y");
    Near(Property(document, 2, "body", "y"), 18, "partial body x edit changed world y");
    Check(document.SetComponentProperty(1, "body", "y", 7.0), "world body y edit failed");
    SceneEditCommand coupled;
    coupled.mutations.push_back({SetComponentPropertyMutation{2, "transform", "x", 5.0}});
    coupled.mutations.push_back({SetComponentPropertyMutation{2, "body", "y", 9.0}});
    Check(document.ApplyEditCommand(coupled), "same-command local/world edits failed");
    Near(Property(document, 2, "body", "x"), 30, "later body edit lost prior world x");
    Near(Property(document, 2, "body", "y"), 9, "later body world y not applied");
    Check(document.SetComponentProperty(1, "body", "rotation", 60.0), "body world rotation edit failed");
    Near(Property(document, 2, "transform", "rotation"), -30, "world rotation not converted to local");
    BodyMatchesWorld(document, 2); BodyMatchesWorld(document, 3);
    Check(document.SetActorParent(1, 3), "rotated child reparent failed");
    Near(Property(document, 2, "body", "x"), 30, "reparent changed world x");
    Near(Property(document, 2, "body", "y"), 9, "reparent changed world y");
    Near(Property(document, 2, "transform", "x"), 31, "reparent local x mismatch");
    BodyMatchesWorld(document, 3);
    Check(document.DeleteActor(3), "delete new parent failed");
    Near(Property(document, 2, "body", "x"), 31, "delete-parent did not retain local pose and update body world x");
    Near(Property(document, 2, "body", "y"), 11, "delete-parent did not update body world y");
    BodyMatchesWorld(document, 2); BodyMatchesWorld(document, 3);
    const auto snapshot = document;
    document.SetPrecommitHook([](const SceneDocument &, const SceneEditCommand &) { return false; });
    Check(!document.SetComponentProperty(1, "transform", "x", 99.0) && Equal(document, snapshot),
          "rejected linked pose edit changed descendant metadata");
    document.SetPrecommitHook({}); Check(document.Save(), "save pose metadata");
    SceneDocument reloaded; Check(reloaded.LoadFromSceneName("poses"), "reload pose metadata");
    BodyMatchesWorld(reloaded, 2); BodyMatchesWorld(reloaded, 3);
}
void DeepLinkedWorldPose(const Fixture &fixture) {
    constexpr int count = 1000;
    std::ofstream output(fixture.root / "scenes/deep.scene"); output << "{\"actors\":[";
    for (int uid = 1; uid <= count; ++uid) {
        if (uid > 1) output << ',';
        output << "{\"uid\":" << uid << ",\"parent_uid\":" << uid - 1
               << ",\"components\":{\"transform\":{\"type\":\"Transform\",\"x\":1,\"rotation\":"
               << (uid == 1 ? 90 : 0) << '}';
        if (uid % 10 == 0) output << ",\"body\":{\"type\":\"Rigidbody\"}";
        output << "}}";
    }
    output << "]}"; output.close();
    SceneDocument document; Check(document.LoadFromSceneName("deep"), "load deep document");
    Check(document.SetComponentProperty(0, "transform", "x", 5.0), "deep root pose edit failed");
    // Float sin/cos composition has the same small per-edge error as the
    // existing world-pose rule; this asserts values, never a timing threshold.
    Check(std::abs(Property(document, count, "body", "x") - 5) < 0.001 &&
          std::abs(Property(document, count, "body", "y") + 999) < 0.001 &&
          Property(document, count, "body", "rotation") == 90,
          "deep descendant world metadata is incorrect");
    const double untouched_y = Property(document, 500, "body", "y");
    Check(document.SetComponentProperty(500, "transform", "x", 2.0), "deep interior pose edit failed");
    Check(Property(document, 500, "body", "y") == untouched_y &&
          std::abs(Property(document, count, "body", "y") + 1000) < 0.001,
          "deep interior edit changed ancestors or lost descendant propagation");
}
void ReceiptCoordination(const Fixture &fixture) {
    auto document = fixture.Document(); int calls = 0;
    document.SetPrecommitHook([&](const SceneDocument &, const SceneEditCommand &command) {
        Check(command.coordinator_receipt == 0, "edit attempt retained old coordinator receipt");
        ++calls; command.coordinator_receipt = 77; return true;
    });
    auto command = Rename("first"); command.coordinator_receipt = 99;
    Check(document.ApplyEditCommand(command) && command.coordinator_receipt == 77 && calls == 1,
          "successful edit lost coordinator receipt");
    Check(!document.ApplyEditCommand(command) && command.coordinator_receipt == 0 && calls == 1,
          "no-op reapplication retained receipt or called hook");
    SceneEditCommand output;
    Check(document.SetActorName(0, "second", &output) && output.coordinator_receipt == 77,
          "public helper lost receipt while returning command");
    document.SetPrecommitHook([](const SceneDocument &, const SceneEditCommand &candidate) {
        candidate.coordinator_receipt = 88; return false;
    });
    command = Rename("rejected");
    Check(!document.ApplyEditCommand(command) && command.coordinator_receipt == 0,
          "rejected hook leaked successful command receipt");
}
void CopiesDetachHooks(const Fixture &fixture) {
    auto original = fixture.Document(); int original_calls = 0;
    original.SetPrecommitHook([&](const SceneDocument &, const SceneEditCommand &) { ++original_calls; return false; });
    SceneDocument copy(original);
    Check(copy.SetActorName(0, "copy") && original_calls == 0, "copy inherited live hook");
    SceneDocument moved(std::move(copy));
    Check(moved.SetActorName(0, "moved") && original_calls == 0, "move inherited source hook");
    auto destination = fixture.Document(); int destination_calls = 0;
    destination.SetPrecommitHook([&](const SceneDocument &, const SceneEditCommand &) { ++destination_calls; return false; });
    destination = original;
    Check(!destination.SetActorName(0, "blocked") && destination_calls == 1 && original_calls == 0, "copy assignment replaced destination hook");
    destination = std::move(moved);
    Check(!destination.SetActorName(0, "blocked") && destination_calls == 2, "move assignment lost destination hook");
    Check(!original.SetActorName(0, "blocked") && original_calls == 1, "copy/move detached original live hook");
}
}
int main() {
    try {
        Fixture fixture; AtomicFailures(fixture); PreparationExceptions(fixture); NineMutationKinds(fixture); PublicPathsAndCounters(fixture); ReceiptCoordination(fixture); CopiesDetachHooks(fixture); LinkedWorldPose(fixture); DeepLinkedWorldPose(fixture);
        std::cout << "scene_document_transaction_test: passed\n"; return 0;
    } catch (const std::exception &error) {
        std::cerr << "scene_document_transaction_test: " << error.what() << '\n'; return 1;
    }
}
