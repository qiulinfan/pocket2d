#include <stdexcept>
#include "scene/Scene.h"
#include "scene/Actor.h"
#include "shared/scene_format/SceneFormat.h"
#include "scripting/ComponentManager.h"
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <utility>

std::filesystem::path Scene::active_scene_subdirectory;

// Override the active scene subdirectory used for resource preference.
void Scene::SetActiveSceneSubdirectory( const std::filesystem::path &subdirectory) {
    active_scene_subdirectory = subdirectory.lexically_normal();
    if (active_scene_subdirectory == ".") {
        active_scene_subdirectory.clear();
    }
}

// relative subdirectory under <project-root>/scenes for the active scene
const std::filesystem::path &Scene::GetActiveSceneSubdirectory() {
    return active_scene_subdirectory;
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
