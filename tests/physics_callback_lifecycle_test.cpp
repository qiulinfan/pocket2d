#include "core/Engine.h"
#include "scene/Tilemap.h"
#include "physics/Rigidbody.h"
#include "physics/RayCast.h"
#include "scripting/ComponentManager.h"
#include "shared/resources/ResourcePath.h"
#include "lua.hpp"
#include "LuaBridge/LuaBridge.h"
#include "SDL3/SDL.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}
struct Fixture {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("pocket2d-callback-life-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixture() {
        for (const char *folder : {"images", "maps", "scenes", "component_types"})
            std::filesystem::create_directories(root / folder);
        ResourcePath::SetResourcesRootPath(root);
        auto *image = SDL_CreateSurface(8, 4, SDL_PIXELFORMAT_RGBA32);
        Check(image != nullptr, "create atlas");
        SDL_FillSurfaceRect(image, nullptr, SDL_MapSurfaceRGBA(image, 255,255,255,255));
        const bool saved = SDL_SaveBMP(image, (root / "images/atlas.bmp").string().c_str());
        SDL_DestroySurface(image); Check(saved, "save atlas");
        TilemapAsset::Data data;
        data.width = 4; data.height = 4; data.atlas = {"atlas.bmp",2,1,4,4};
        data.ground.cells.assign(16,1); data.solid.cells.assign(16,0);
        for (int y = 0; y < 4; ++y) data.solid.cells[y * 4 + 2] = 1;
        TilemapAsset::ValidateAtlasImage(data, root / "images");
        TilemapAsset::SaveResource("wall.tilemap.json", data);
        std::ofstream(root / "game.config") << R"({"initial_scene":"callback"})";
        std::ofstream(root / "scenes/callback.scene") << R"({"actors":[
            {"uid":2,"name":"player","components":{
                "body":{"type":"Rigidbody","gravity_scale":0,"has_trigger":false,"collider_type":"circle","radius":0.25},
                "probe":{"type":"DestroyProbe"},
                "transform":{"type":"Transform","x":1.5,"y":1.5}}},
            {"uid":10,"name":"terrain","components":{
                "map":{"type":"Tilemap","map_resource":"wall.tilemap.json"},"transform":{"type":"Transform"}}}
        ]})";
        std::ofstream(root / "component_types/DestroyProbe.lua") << R"(
            DestroyProbe = {
                called=0, cleanup_mode=0, map_rejected=false, body_rejected=false,
                OnStart=function(self)
                    self.body=self.actor:GetComponent("Rigidbody")
                    self.terrain=Actor.Find("terrain")
                    self.map=self.terrain:GetComponent("Tilemap")
                end,
                OnUpdate=function(self)
                    if self.called == 0 then self.body:SetVelocity(Vector2(3,0)) end
                end,
                OnCollisionEnter=function(self,collision)
                    if self.called ~= 0 or collision.other ~= self.terrain then return end
                    self.called=1
                    assert(self.map:SetCell("ground",0,0,2))
                    local map_ok,map_error=pcall(function() self.map:OnDestroy() end)
                    self.map_rejected=not map_ok and string.find(map_error,"RemoveComponent") ~= nil
                    local body_ok,body_error=pcall(function() self.body:OnDestroy() end)
                    self.body_rejected=not body_ok and string.find(body_error,"Actor.Destroy") ~= nil
                    assert(self.map_rejected and self.body_rejected)
                    assert(self.map:GetCell("ground",0,0) == 1)
                    local hit=Physics.Raycast(Vector2(2.5,-1),Vector2(0,1),8)
                    assert(hit ~= nil and hit.actor == self.terrain)
                    if self.cleanup_mode == 1 then
                        self.terrain:RemoveComponent(self.map)
                        self.actor:RemoveComponent(self.body)
                    elseif self.cleanup_mode == 2 then
                        Actor.Destroy(self.terrain)
                        Actor.Destroy(self.actor)
                    end
                end
            }
        )";
    }
    ~Fixture() { std::error_code error; std::filesystem::remove_all(root,error); }
};
void CheckMode(int mode) {
    Engine engine; engine.InitializeHeadless();
    auto probe = ComponentManager::GetComponentByKey(2,"probe");
    auto body_reference = ComponentManager::GetComponentByKey(2,"body");
    auto map_reference = ComponentManager::GetComponentByKey(10,"map");
    auto *body = body_reference.cast<Rigidbody *>().value();
    auto *map = map_reference.cast<Tilemap *>().value();
    Check(ComponentManager::SetRuntimeComponentPropertyValue(2,"probe","cleanup_mode",mode), "set cleanup mode");
    for (int i = 0; i < 60 && probe["called"].cast<int>().value() == 0; ++i) {
        engine.StepSimulation();
        Check(ComponentManager::GetRuntimeErrorCount() == 0, ComponentManager::GetLastRuntimeError());
    }
    Check(probe["called"].cast<int>().value() == 1 && probe["map_rejected"].cast<bool>().value() &&
          probe["body_rejected"].cast<bool>().value(), "real callback must reject both direct lifecycle calls");
    if (mode == 0) {
        Check(map->HasAsset() && map->GetFixtureCount() == 4 && body->HasBody(),
              "rejected calls must preserve bodies and map state");
        const auto hit = RayCast::Cast({2.5f,-1},{0,1},8);
        Check(hit && hit->actor == engine.GetRuntimeActorByUID(10), "rejected calls must preserve raycast metadata");
        Check(map->GetCell("ground",0,0) == 1, "callback edit stays pending this frame");
        engine.StepSimulation();
        Check(map->GetCell("ground",0,0) == 2, "rejected destroy must preserve queued cell edits");
    } else {
        Check(!body->HasBody() && !map->HasAsset() && map->GetFixtureCount() == 0,
              "authorized deferred cleanup must finish outside locked world");
        Check(RayCast::GetBodyCount() == 0, "deferred cleanup must leave no orphan bodies");
        Check(!RayCast::Cast({2.5f,-1},{0,1},8), "deferred cleanup must unregister fixtures");
        if (mode == 1)
            Check(engine.GetRuntimeActorByUID(2) && engine.GetRuntimeActorByUID(10),
                  "RemoveComponent retains owning actors");
        else
            Check(!engine.GetRuntimeActorByUID(2) && !engine.GetRuntimeActorByUID(10),
                  "Actor.Destroy removes owning actors");
        engine.StepSimulation();
    }
    Check(ComponentManager::GetRuntimeErrorCount() == 0, ComponentManager::GetLastRuntimeError());
}
} // namespace
int main() {
    try {
        Fixture fixture;
        CheckMode(0); CheckMode(1); CheckMode(2);
        std::cout << "PASS locked-world lifecycle rejection and deferred component/actor cleanup\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL physics callback lifecycle: " << error.what() << '\n'; return 1;
    }
}
