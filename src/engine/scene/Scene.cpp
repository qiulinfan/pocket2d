#include <stdexcept>
#include "scene/Scene.h"
#include "core/World.h"
#include "scene/Actor.h"
#include "shared/scene_format/SceneFormat.h"
#include "scripting/ComponentManager.h"
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <utility>

namespace {
struct ActiveScene { std::filesystem::path subdirectory; };
std::filesystem::path &ActiveSubdirectory() { return World::Current().State<ActiveScene>().subdirectory; }
} // namespace

// Override the active scene subdirectory used for resource preference.
void Scene::SetActiveSceneSubdirectory( const std::filesystem::path &subdirectory) {
    std::filesystem::path &active = ActiveSubdirectory();
    active = subdirectory.lexically_normal();
    if (active == ".") {
        active.clear();
    }
}

// relative subdirectory under <project-root>/scenes for the active scene
const std::filesystem::path &Scene::GetActiveSceneSubdirectory() {
    return ActiveSubdirectory();
}

// load actors from <project-root>/scenes/<scene_name>.scene
std::vector<Actor> Scene::LoadScene(const std::string &scene_name) {
    const auto asset = SceneFormat::LoadSceneAsset(scene_name,GetActiveSceneSubdirectory());
    std::string error;
    if (!SceneFormat::ValidateSceneAssetForRuntime(asset,&error)) throw std::runtime_error(error);
    // Data-only loading cannot execute component scripts, bind instances, or
    // change the current world's resource preference. Engine owns transitions.
    return SceneFormat::BuildRuntimeActors(asset);
}
