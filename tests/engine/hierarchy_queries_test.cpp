#include "scripting/ComponentManager.h"
#include "scene/Transform.h"
#include "shared/resources/ResourcePath.h"
#include "lua.hpp"
#include "LuaBridge/LuaBridge.h"
#include <cmath>
#include <list>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void Check(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

Actor MakeActor(Actor::UID uid, Actor::UID parent = Actor::kInvalidUID) {
    Actor actor;
    actor.uid = uid;
    actor.parent_uid = parent;
    actor.actor_name = "actor_" + std::to_string(uid);
    return actor;
}

void AddTransform(Actor::UID uid, const std::string &key = "transform",
                  double x = 1.0, double y = 0.0, double rotation = 0.0) {
    ComponentManager::InstantiateComponentForActor(
        uid, {key, "Transform", {{"x", x}, {"y", y}, {"rotation", rotation}}});
}

std::vector<std::string> Keys(const luabridge::LuaRef &result) {
    std::vector<std::string> keys;
    for (int i = 1; i <= result.length(); ++i) {
        luabridge::LuaRef component = result[i];
        Transform *transform = component.cast<Transform *>().value();
        keys.push_back(std::to_string(transform->actor->uid) + ":" + transform->key);
    }
    return keys;
}

void CheckWorld(Actor::UID uid, float expected_x, float expected_y,
                 float expected_rotation = 0.0f) {
    float x = 0.0f, y = 0.0f, rotation = 0.0f;
    Check(ComponentManager::TryGetRuntimeTransformWorld(uid, x, y, rotation),
          "world transform must resolve");
    Check(std::abs(x - expected_x) < 0.001f &&
              std::abs(y - expected_y) < 0.001f &&
              std::abs(rotation - expected_rotation) < 0.001f,
          "world pose mismatch");
}

void TestSubtreeOrderingAndMutation() {
    std::list<Actor> actors{MakeActor(3, 2), MakeActor(1),
                             MakeActor(4), MakeActor(2, 1)};
    AddTransform(3, "z");
    AddTransform(3, "a");
    for (Actor::UID uid : {1, 2, 4}) AddTransform(uid, "a");
    ComponentManager::BindActorsForScene(actors);
    Check(Keys(ComponentManager::GetComponentsInChildrenByType(1, "Transform")) ==
              std::vector<std::string>({"3:a", "3:z", "1:a", "2:a"}),
          "subtree must preserve scene order then key order");
    auto first = ComponentManager::GetComponentInChildrenByType(1, "Transform");
    Check(first.cast<Transform *>().value()->actor->uid == 3 &&
              first.cast<Transform *>().value()->key == "a", "first query ordering changed");
    Check(ComponentManager::GetChildCount(1) == 1, "direct child count mismatch");
    auto children = ComponentManager::GetChildren(1);
    Check(children.length() == 1 && children[1].cast<ActorHandle>().value().GetUID() == 2,
          "children must exclude grandchildren");
    Check(ComponentManager::GetComponentInChildrenByType(999, "Transform").isNil(),
          "unrelated missing root must return nil");
    Check(ComponentManager::GetComponentsInChildrenByType(0, "Transform").length() == 0,
          "invalid root must return empty array");
    Check(ComponentManager::GetComponentInChildrenByType(1, "").isNil(),
          "empty component type must return nil");

    auto removed = ComponentManager::GetComponentByKey(3, "a");
    ComponentManager::RemoveComponent(3, removed);
    Check(Keys(ComponentManager::GetComponentsInChildrenByType(1, "Transform")) ==
              std::vector<std::string>({"3:z", "1:a", "2:a"}),
          "removed components must disappear before frame finalization");

    // A direct parent write is intentionally tested: queries cannot depend on
    // mutation APIs being the only writers of the public Actor fields.
    actors.front().parent_uid = 4;
    Check(Keys(ComponentManager::GetComponentsInChildrenByType(1, "Transform")) ==
              std::vector<std::string>({"1:a", "2:a"}),
          "parent edits must be visible to the next query");
    actors.front().parent_uid = 1;
    Check(ComponentManager::GetComponentsInChildrenByType(1, "Transform").length() == 3,
          "second parent edit must update subtree membership");
    ComponentManager::DestroyActor(&actors.front());
    ComponentManager::FinalizeFrameMutations();
    Check(ComponentManager::GetComponentsInChildrenByType(1, "Transform").length() == 2,
          "destroyed actor must leave query results");
}

void TestCyclesAndMissingParents() {
    std::list<Actor> actors{MakeActor(1, 2), MakeActor(2, 1),
                             MakeActor(3, 999), MakeActor(4, 4)};
    for (const Actor &actor : actors) AddTransform(actor.uid);
    ComponentManager::BindActorsForScene(actors);
    Check(ComponentManager::GetComponentsInChildrenByType(1, "Transform").length() == 2,
          "cycle query must terminate and deduplicate");
    Check(ComponentManager::GetComponentsInChildrenByType(999, "Transform").length() == 1,
          "missing root retains descendants that name it as parent");
    Check(ComponentManager::GetComponentsInChildrenByType(4, "Transform").length() == 1,
          "self-parent must not duplicate results");
    CheckWorld(3, 1.0f, 0.0f);
    CheckWorld(4, 1.0f, 0.0f);
    float x, y, rotation;
    Check(ComponentManager::TryGetRuntimeTransformWorld(1, x, y, rotation) &&
              std::isfinite(x), "cyclic transform query must terminate");
    ComponentManager::ResolveTransformHierarchy();
}

void TestWorldTransformsAndImmediateLuaWrites() {
    std::list<Actor> actors{MakeActor(3, 2), MakeActor(2, 1),
                             MakeActor(1), MakeActor(4, 5), MakeActor(5, 1)};
    AddTransform(1, "root", 10.0, 20.0, 90.0);
    AddTransform(2, "child", 2.0, 0.0, 10.0);
    AddTransform(3, "leaf", 0.0, 0.0, 5.0);
    AddTransform(4, "orphan", 7.0, 8.0);
    ComponentManager::BindActorsForScene(actors);
    CheckWorld(2, 10.0f, 18.0f, 100.0f);
    CheckWorld(3, 10.0f, 18.0f, 105.0f);
    CheckWorld(4, 7.0f, 8.0f); // A parent without Transform breaks inheritance.

    auto root = ComponentManager::GetComponentByKey(1, "root");
    luabridge::setGlobal(root.state(), root, "hierarchy_test_root");
    Check(luaL_dostring(root.state(), "hierarchy_test_root.x = 25") == LUA_OK,
          "Lua transform assignment failed");
    CheckWorld(3, 25.0f, 18.0f, 105.0f);
    actors.front().parent_uid = 1;
    CheckWorld(3, 25.0f, 20.0f, 95.0f);
    Check(ComponentManager::SetRuntimeTransformWorldPosition(3, 30.0f, 40.0f),
          "world position assignment failed");
    CheckWorld(3, 30.0f, 40.0f, 95.0f);
    ComponentManager::ResolveTransformHierarchy();
    auto leaf = ComponentManager::GetComponentByKey(3, "leaf");
    Check(std::abs(leaf.cast<Transform *>().value()->world_x - 30.0f) < 0.001f,
          "full-scene and single-root resolution disagree");
    float x, y, rotation;
    Check(!ComponentManager::TryGetRuntimeTransformWorld(5, x, y, rotation),
          "actor without Transform must fail lookup");
    lua_pushnil(root.state());
    lua_setglobal(root.state(), "hierarchy_test_root");
}

void TestDeepHierarchy() {
    constexpr int count = 12000;
    std::list<Actor> actors;
    for (int i = 1; i <= count; ++i) {
        actors.push_back(MakeActor(i, i - 1));
        AddTransform(i);
    }
    ComponentManager::BindActorsForScene(actors);
    CheckWorld(count, static_cast<float>(count), 0.0f);
    ComponentManager::ResolveTransformHierarchy();
    Check(ComponentManager::GetComponentsInChildrenByType(1, "Transform").length() == count,
          "deep hierarchy must return every component without recursion");
}
} // namespace

int main() {
    ResourcePath::SetResourcesRootPath("__hierarchy_test_no_project__");
    ComponentManager::Initialize();
    try {
        for (auto test : {TestSubtreeOrderingAndMutation, TestCyclesAndMissingParents,
                          TestWorldTransformsAndImmediateLuaWrites, TestDeepHierarchy}) {
            test();
            ComponentManager::ClearActorComponents();
        }
        ComponentManager::Shutdown();
        std::cout << "hierarchy query and transform regressions passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        ComponentManager::Shutdown();
        return 1;
    }
}
