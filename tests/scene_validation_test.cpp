#include "shared/scene_format/SceneFormat.h"
#include "shared/config/GameConfig.h"
#include "shared/resources/ResourcePath.h"
#include "scene/Scene.h"
#include "editor/documents/SceneDocument.h"
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace {
void Check(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

void Write(const std::filesystem::path &path, const std::string &text) {
    std::ofstream output(path);
    output << text;
    Check(output.good(), "failed to write test fixture");
}

std::string Read(const std::filesystem::path &path) {
    std::ifstream input(path);
    std::ostringstream content;
    content << input.rdbuf();
    return content.str();
}

template <typename Fn> void Rejects(Fn operation, const std::string &fragment) {
    try {
        operation();
    } catch (const std::runtime_error &error) {
        Check(std::string(error.what()).find(fragment) != std::string::npos,
              "validation error must identify the offending field or asset");
        return;
    }
    throw std::runtime_error("invalid input was silently accepted: " + fragment);
}

class ProjectFixture {
public:
    std::filesystem::path root;
    ProjectFixture() : previous_root_(ResourcePath::ResourcesRootPath()),
                       previous_scene_(Scene::GetActiveSceneSubdirectory()) {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        root = std::filesystem::temp_directory_path() /
               ("pocket2d-scene-validation-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(root), "temporary directory collision");
        std::filesystem::create_directory(root / "scenes");
        std::filesystem::create_directory(root / "actor_templates");
        ResourcePath::SetResourcesRootPath(root);
        Scene::SetActiveSceneSubdirectory({});
    }
    ~ProjectFixture() {
        Actor::ClearTemplateCache();
        ResourcePath::SetResourcesRootPath(previous_root_);
        Scene::SetActiveSceneSubdirectory(previous_scene_);
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
private:
    std::filesystem::path previous_root_;
    std::filesystem::path previous_scene_;
};

void TestMalformedFields(const ProjectFixture &project) {
    const auto path = project.root / "scenes" / "invalid.scene";
    const std::pair<const char *, const char *> cases[] = {
        {R"({"actors":false})", "actors"},
        {R"({"actors":[3]})", "actors[0]"},
        {R"({"actors":[{"uid":-1}]})", ".uid"},
        {R"({"actors":[{"components":[]}]})", ".components"},
        {R"({"actors":[{"components":{"logic":false}}]})", ".logic"},
        {R"({"actors":[{"components":{"logic":{"type":3}}}]})", ".type"},
        {R"({"actors":[{"components":{"logic":{"type":"Probe","values":[1,"wrong"]}}}]})", ".values"},
        {R"({"actors":[{"components":{"logic":{"type":"Probe","values":{"__array_type":"int","items":[2147483648]}}}}]})", ".values"},
        {R"({"actors":[{"components":{"logic":{"type":"Probe","type":"Other"}}}]})", "duplicate field"},
        {R"({"actors":[{"components":{"same":{},"same":{}}}]})", "duplicate field"}
    };
    for (const auto &[source, fragment] : cases) {
        Write(path, source);
        Rejects([] { SceneFormat::LoadSceneAsset("invalid"); }, fragment);
    }
}

void TestIdentityAssignment(const ProjectFixture &project) {
    const auto path = project.root / "scenes" / "identity.scene";
    Write(path, R"({"actors":[{}, {"uid":2}, {"uid":0}, {"uid":4}]})");
    auto scene = SceneFormat::LoadSceneAsset("identity");
    Check(scene.actors.size() == 4, "legacy actors must remain loadable");
    for (std::size_t i = 0; i < scene.actors.size(); ++i)
        Check(scene.actors[i].uid == i + 1, "missing IDs must be assigned without colliding with explicit IDs");
    Check(SceneFormat::LoadSceneAsset("identity").actors[0].uid == 1,
          "missing-ID assignment must be deterministic across loads");
    auto raw = scene;
    for (auto &actor : raw.actors) actor.uid = 0;
    auto runtime = SceneFormat::BuildRuntimeActors(raw);
    for (std::size_t i = 0; i < runtime.size(); ++i)
        Check(runtime[i].uid == i + 1, "programmatic scenes must receive valid runtime IDs too");
    Write(path, R"({"actors":[{"uid":7},{"uid":7}]})");
    Rejects([] { SceneFormat::LoadSceneAsset("identity"); }, "duplicate actor uid 7");
    scene.actors[1].uid = scene.actors[0].uid;
    std::string error;
    Check(!SceneFormat::ValidateSceneAssetForRuntime(scene, &error) &&
              error.find("duplicate actor uid") != std::string::npos,
          "runtime validation must reject duplicate IDs in memory");
}

void TestTemplatesAndRoundTrip(const ProjectFixture &project) {
    Write(project.root / "actor_templates" / "Base.template", R"({
        "components":{"body":{"type":"Transform","x":1},
                      "removed":{"type":"SpriteRenderer","sprite":"test"}}
    })");
    Write(project.root / "scenes" / "valid.scene", R"({"actors":[{
        "template":"Base", "components":{
            "body":{"x":4},
            "removed":{"type":"__editor_deleted__"},
            "logic":{"type":"Probe","values":{"__array_type":"double","items":[]}}
        }
    }]})");
    auto scene = SceneFormat::LoadSceneAsset("valid");
    std::string error;
    Check(SceneFormat::ValidateSceneAssetForRuntime(scene, &error), "valid template reference rejected");
    const auto actors = SceneFormat::BuildRuntimeActors(scene);
    const auto *body = SceneFormat::FindComponentSpec(actors.front().component_specs, "body");
    Check(body != nullptr && body->type == "Transform", "typeless scene override must inherit its template type");
    Check(std::get<int>(body->overrides.front().value) == 4, "scene override must replace template value");
    Check(SceneFormat::FindComponentSpec(actors.front().component_specs, "removed") == nullptr,
          "editor deletion tombstone must retain its behavior");
    Check(SceneFormat::SaveSceneAsset(scene), "valid scene write failed");
    const auto roundtrip = SceneFormat::LoadSceneAsset("valid");
    const auto *logic = SceneFormat::FindComponentSpec(roundtrip.actors.front().component_specs, "logic");
    Check(logic && std::holds_alternative<Actor::DoubleArray>(logic->overrides.front().value) &&
              std::get<Actor::DoubleArray>(logic->overrides.front().value).empty(),
          "typed empty arrays must retain type across roundtrip");

    auto missing = scene;
    missing.actors.front().template_name = "Absent";
    Check(!SceneFormat::ValidateSceneAssetForRuntime(missing, &error) &&
              error.find("missing template") != std::string::npos,
          "missing referenced template must produce a validation message");
    Check(!SceneFormat::ValidateSceneAssetForRuntime(SceneFormat::LoadSceneAsset("absent"), &error),
          "missing scene must fail runtime validation");
    Write(project.root / "actor_templates" / "Recursive.template", R"({"template":"Recursive"})");
    Rejects([] { SceneFormat::LoadActorTemplateAsset("Recursive"); }, "template inheritance is not supported");
}

void TestConfiguration(const ProjectFixture &project) {
    const auto game = project.root / "game.config";
    const auto render = project.root / "rendering.config";
    Write(game, "{}");
    Check(GameConfig::Read().window_width == 640, "omitted config settings must retain defaults");
    Write(game, R"({"initial_scene":12})");
    Rejects([] { GameConfig::Read(); }, "initial_scene");
    Write(game, R"({"game_title":"a","game_title":"b"})");
    Rejects([] { GameConfig::Read(); }, "duplicate field");
    Write(game, "{}");
    Check(!GameConfig::Read().splash_screen, "the splash screen is off unless a project asks for it");
    Write(game, R"({"splash_screen":"yes"})");
    Rejects([] { GameConfig::Read(); }, "splash_screen");
    Write(game, R"({"splash_screen":true,"splash_seconds":30})");
    Rejects([] { GameConfig::Read(); }, "splash_seconds");
    Write(game, R"({"splash_screen":true,"splash_seconds":3})");
    Check(GameConfig::Read().splash_screen && GameConfig::Read().splash_seconds == 3.0f,
          "a project's splash screen settings must be read");
    Write(game, R"({"show_fps":1})");
    Rejects([] { GameConfig::Read(); }, "show_fps");
    Write(game, R"({"show_fps":true})");
    Check(GameConfig::Read().show_fps, "a project's show_fps setting must be read");
    Write(game, R"({"initial_scene":"valid","game_title":"test"})");
    const std::pair<const char *, const char *> cases[] = {
        {R"({"x_resolution":"wide"})", "x_resolution"},
        {R"({"y_resolution":0})", "y_resolution"},
        {R"({"clear_color_r":256})", "clear_color_r"},
        {R"({"zoom_factor":1e300})", "zoom_factor"},
        {R"({"zoom_factor":-1})", "zoom_factor"}
    };
    for (const auto &[source, fragment] : cases) {
        Write(render, source);
        Rejects([] { GameConfig::Read(); }, fragment);
    }
    Write(render, R"({"x_resolution":800,"y_resolution":600,"zoom_factor":1.5})");
    const auto config = GameConfig::Read();
    Check(config.window_width == 800 && config.window_height == 600 && config.zoom_factor == 1.5f,
          "valid rendering config must retain its values");
}

void TestInvalidWritesPreserveFiles(const ProjectFixture &project) {
    auto scene = SceneFormat::LoadSceneAsset("valid");
    const std::string original = Read(scene.scene_path);
    scene.actors.front().component_specs.front().overrides.push_back(
        {"invalid", std::numeric_limits<double>::infinity()});
    Check(!SceneFormat::SaveSceneAsset(scene), "non-finite scene values must be rejected before writing");
    Check(Read(scene.scene_path) == original, "failed scene save must not truncate existing data");
    scene.actors.front().component_specs.front().overrides.back().value =
        Actor::DoubleArray{1.0, std::numeric_limits<double>::quiet_NaN()};
    Check(!SceneFormat::SaveSceneAsset(scene) && Read(scene.scene_path) == original,
          "non-finite array elements must also fail without truncating the scene");
    auto config = GameConfig::Read();
    const std::string original_config = Read(project.root / "rendering.config");
    config.zoom_factor = std::numeric_limits<float>::infinity();
    Check(!GameConfig::Write(config), "non-finite config value must fail write");
    Check(Read(project.root / "rendering.config") == original_config,
          "failed config save must preserve existing data");
}

void TestEditorParticleSentinel(const ProjectFixture &project) {
    Write(project.root / "scenes" / "particle_edit.scene", R"({"actors":[{}]})");
    SceneDocument document;
    Check(document.LoadFromSceneName("particle_edit"), "editor scene load failed");
    Check(document.AddComponentToActor(0, "ParticleSystem"), "editor add ParticleSystem failed");
    const auto &component = document.GetActorRecords().front().component_specs.front();
    const std::string key = component.key;
    bool found_unset_default = false;
    for (const auto &property : document.GetInspectableProperties(0, key)) {
        if (property.name == "end_scale") {
            found_unset_default = std::isnan(std::get<double>(property.value));
        }
    }
    Check(found_unset_default, "test must exercise the real editor's NaN default metadata");
    Check(document.Save(), "adding a default ParticleSystem must remain saveable");
    Check(document.SetComponentProperty(0, key, "end_scale", std::numeric_limits<double>::quiet_NaN()),
          "editor unset end-scale edit failed");
    Check(document.Save(), "unset ParticleSystem end_scale must serialize by omission");
    const auto reloaded = SceneFormat::LoadSceneAsset("particle_edit");
    for (const auto &property : reloaded.actors.front().component_specs.front().overrides)
        Check(property.name != "end_scale", "unset end_scale must not write a non-JSON NaN token");
    const auto path = project.root / "scenes" / "particle_edit.scene";
    const std::string valid_file = Read(path);
    Check(document.SetComponentProperty(0, key, "end_scale", std::numeric_limits<double>::infinity()),
          "editor infinity test setup failed");
    Check(!document.Save() && Read(path) == valid_file,
          "infinite particle end_scale must still fail without corrupting the scene");
}
} // namespace

int main() {
    try {
        ProjectFixture project;
        TestMalformedFields(project);
        TestIdentityAssignment(project);
        TestTemplatesAndRoundTrip(project);
        TestConfiguration(project);
        TestInvalidWritesPreserveFiles(project);
        TestEditorParticleSentinel(project);
        std::cout << "scene/config validation and legacy-format regressions passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
