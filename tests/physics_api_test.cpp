// Lua physics API beyond Rigidbody basics: queries, gravity, collision layers,
// capsule/polygon colliders, impulses, joints, the kinematic character mover,
// and smooth sliding along tilemap outlines.
#include "core/Engine.h"
#include "physics/Rigidbody.h"
#include "scripting/ComponentManager.h"
#include "scripting/LuaBoundary.h"
#include "shared/resources/ResourcePath.h"
#include "shared/tilemap/TilemapAsset.h"
#include "lua.hpp"
#include "LuaBridge/LuaBridge.h"
#include <SDL3/SDL.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
namespace {
void Check(bool condition, const std::string &message) { if (!condition) throw std::runtime_error(message); }
void Lua(lua_State *state, const std::string &source) {
    const int top = lua_gettop(state);
    if (luaL_loadstring(state, source.c_str()) != LUA_OK) {
        const std::string error = lua_tostring(state, -1); lua_settop(state, top); throw std::runtime_error(error);
    }
    LuaBoundary::ProtectedCall(state, 0, 0);
    lua_settop(state, top);
}
void Step(Engine &engine, int frames) {
    for (int i = 0; i < frames; ++i) engine.StepSimulation();
    Check(ComponentManager::GetRuntimeErrorCount() == 0, ComponentManager::GetLastRuntimeError());
}

struct Fixture {
    fs::path previous = ResourcePath::ResourcesRootPath();
    fs::path root = fs::temp_directory_path() /
        ("pocket-physics-api-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixture() {
        for (const char *directory : {"scenes", "component_types", "images", "maps"}) fs::create_directories(root / directory);
        auto *atlas = SDL_CreateSurface(2, 2, SDL_PIXELFORMAT_RGBA32);
        SDL_FillSurfaceRect(atlas, nullptr, SDL_MapSurfaceRGBA(atlas, 200, 200, 200, 255));
        Check(SDL_SaveBMP(atlas, (root / "images/atlas.bmp").string().c_str()), "save atlas");
        SDL_DestroySurface(atlas);
        ResourcePath::SetResourcesRootPath(root);
        // A 20-cell floor row at y=10 for sliding, and a wall column at x=30.
        TilemapAsset::Data map;
        map.width = 40; map.height = 12; map.atlas = {"atlas.bmp", 1, 1, 2, 2};
        map.ground.cells.assign(40 * 12, 0); map.solid.cells.assign(40 * 12, 0);
        for (int x = 0; x < 20; ++x) map.solid.cells[10 * 40 + x] = 1;
        for (int y = 0; y < 12; ++y) map.solid.cells[y * 40 + 30] = 1;
        TilemapAsset::ValidateAtlasImage(map, root / "images");
        TilemapAsset::SaveResource("floor.tilemap.json", map);
        std::ofstream(root / "game.config") << R"({"initial_scene":"main"})";
        std::ofstream(root / "scenes/main.scene") << R"({"actors":[
          {"uid":1,"name":"a","components":{"body":{"type":"Rigidbody","x":0,"y":0,"gravity_scale":0,"has_trigger":false}}},
          {"uid":2,"name":"b","components":{"body":{"type":"Rigidbody","x":3,"y":0,"gravity_scale":0,"has_trigger":false}}},
          {"uid":3,"name":"sensor","components":{"body":{"type":"Rigidbody","body_type":"static","x":0,"y":5,"has_collider":false,"has_trigger":true}}},
          {"uid":4,"name":"capsule","components":{"body":{"type":"Rigidbody","x":-5,"y":0,"gravity_scale":0,"has_trigger":false,"collider_type":"capsule","radius":0.25,"height":1.5}}},
          {"uid":5,"name":"polygon","components":{"body":{"type":"Rigidbody","x":-8,"y":0,"gravity_scale":0,"has_trigger":false,"collider_type":"polygon","collider_points":[0.0,-0.5,0.5,0.5,-0.5,0.5]}}},
          {"uid":6,"name":"ghost","components":{"body":{"type":"Rigidbody","x":6,"y":0,"gravity_scale":0,"has_trigger":false,"collision_layer":2,"collision_mask":2}}},
          {"uid":7,"name":"slider","components":{"body":{"type":"Rigidbody","x":1,"y":9.5,"gravity_scale":0,"has_trigger":false,"width":0.9,"height":0.9,"friction":0}}},
          {"uid":8,"name":"mover","components":{"transform":{"type":"Transform","x":28,"y":5},"body":{"type":"Rigidbody","body_type":"kinematic","x":28,"y":5,"has_trigger":false,"collider_type":"circle","radius":0.4}}},
          {"uid":9,"name":"terrain","components":{"map":{"type":"Tilemap","map_resource":"floor.tilemap.json"},"transform":{"type":"Transform"}}},
          {"uid":10,"name":"boxer","components":{"transform":{"type":"Transform","x":5,"y":9.1},"body":{"type":"Rigidbody","body_type":"kinematic","x":5,"y":9.1,"has_trigger":false,"collider_type":"box","width":0.8,"height":1.6}}},
          {"uid":11,"name":"pillar","components":{"body":{"type":"Rigidbody","body_type":"static","x":10,"y":5,"has_trigger":false,"width":1,"height":6}}},
          {"uid":12,"name":"pen","components":{"body":{"type":"Rigidbody","body_type":"static","x":48,"y":5,"has_trigger":false,"width":2,"height":8}}},
          {"uid":13,"name":"pen","components":{"body":{"type":"Rigidbody","body_type":"static","x":52,"y":5,"has_trigger":false,"width":2,"height":8}}},
          {"uid":14,"name":"pen","components":{"body":{"type":"Rigidbody","body_type":"static","x":50,"y":2,"has_trigger":false,"width":6,"height":2}}},
          {"uid":15,"name":"pen","components":{"body":{"type":"Rigidbody","body_type":"static","x":50,"y":8,"has_trigger":false,"width":6,"height":2}}}
        ]})";
    }
    ~Fixture() { ResourcePath::SetResourcesRootPath(previous); std::error_code error; fs::remove_all(root, error); }
};

void Run() {
    Fixture fixture;
    Engine engine;
    Check(engine.GetLastSceneLoadError().empty(), engine.GetLastSceneLoadError());
    engine.InitializeHeadless();
    Step(engine, 1);
    lua_State *state = ComponentManager::GetComponentByKey(1, "body").state();
    Lua(state, R"(
      A=Actor.Find('a'):GetComponent('Rigidbody'); B=Actor.Find('b'):GetComponent('Rigidbody')
      CAP=Actor.Find('capsule'):GetComponent('Rigidbody'); POLY=Actor.Find('polygon'):GetComponent('Rigidbody')
      GHOST=Actor.Find('ghost'):GetComponent('Rigidbody'); SLIDER=Actor.Find('slider'):GetComponent('Rigidbody')
      MOVER=Actor.Find('mover'):GetComponent('Rigidbody')
      BOXER=Actor.Find('boxer'):GetComponent('Rigidbody')
      local function names(list) local out={} for i,a in ipairs(list) do out[i]=a:GetName() end return table.concat(out,',') end
      assert(names(Physics.OverlapCircle(Vector2(0,0),0.6,false))=='a','overlap circle finds colliders')
      assert(names(Physics.OverlapCircle(Vector2(0,5),0.3,false))=='','triggers are excluded by default')
      assert(names(Physics.OverlapCircle(Vector2(0,5),0.3,true))=='sensor','triggers can be included')
      assert(names(Physics.OverlapBox(Vector2(1.5,0),5,1,false))=='a,b','box overlap returns each actor once in UID order')
      local hit=Physics.CircleCast(Vector2(-1.5,0),0.2,Vector2(1,0),10)
      assert(hit~=nil and hit.actor:GetName()=='a' and math.abs(hit.point.x+0.5)<0.05,'circle cast hits the nearest collider')
      assert(Physics.CircleCast(Vector2(-1.5,3),0.2,Vector2(1,0),2)==nil,'circle cast misses')
      local g=Physics.GetGravity(); assert(g.x==0 and math.abs(g.y-9.8)<1e-6,'default gravity')
      assert(CAP:GetMass()>0 and POLY:GetMass()>0,'capsule and polygon colliders have mass')
      assert(math.abs(POLY:GetMass()-0.5)<1e-3,'polygon mass is its hull area times density')
      local ok=pcall(function() POLY.collider_points={0,0,1} end); assert(not ok,'odd point lists are rejected')
      ok=pcall(function() GHOST.collision_layer=40 end); assert(not ok,'layers above 31 are rejected')
      -- impulse and torque on a free body
      A:ApplyImpulse(Vector2(A:GetMass()*2,0)); A:AddTorque(5)
    )");
    Step(engine, 1);
    Lua(state, R"(
      local v=A:GetVelocity(); assert(math.abs(v.x-2)<1e-3,'impulse sets velocity by impulse/mass: '..v.x)
      assert(A:GetAngularVelocity()>0,'clockwise torque spins clockwise')
      A:SetVelocity(Vector2(0,0)); A:SetAngularVelocity(0)
      -- layers: ghost (layer 2 only) passes through b (layer 1)
      GHOST:SetVelocity(Vector2(-6,0))
    )");
    Step(engine, 50);
    Lua(state, R"(
      assert(GHOST:GetPosition().x<1.5,'bodies on unrelated layers pass through each other: '..GHOST:GetPosition().x)
      assert(math.abs(B:GetPosition().x-3)<1e-3,'the other body was not pushed')
      -- joints
      J=Physics.ConnectDistance(A,B,A:GetPosition(),B:GetPosition())
      assert(J:IsValid() and J:GetKind()=='distance','distance joint')
      B:SetVelocity(Vector2(0,5))
    )");
    Step(engine, 30);
    Lua(state, R"(
      local d=Vector2.Distance(A:GetPosition(),B:GetPosition())
      assert(math.abs(d-3)<0.05,'distance joint keeps length: '..d)
      J:SetLimits(true,1,3.5); J:SetSpring(true,4,0.7)
      J:Destroy(); assert(not J:IsValid(),'destroyed joint is invalid')
      R=Physics.ConnectRevolute(A,B,A:GetPosition()); R:SetMotor(true,90,1000); R:SetLimits(true,-45,45)
      W=Physics.ConnectWeld(CAP,POLY,CAP:GetPosition()); W:SetSpring(true,5,1)
      P=Physics.ConnectPrismatic(A,CAP,A:GetPosition(),Vector2(1,0)); P:SetLimits(true,-1,1); P:SetMotor(true,1,10)
      WH=Physics.ConnectWheel(B,GHOST,B:GetPosition(),Vector2(0,1)); WH:SetSpring(true,3,0.5)
      M=Physics.ConnectMotor(A,POLY); M:SetMotor(true,0,50)
      for _,j in ipairs({R,W,P,WH,M}) do assert(j:IsValid(),j:GetKind()..' joint valid') end
      ok=pcall(function() W:SetLimits(true,0,1) end); assert(not ok,'weld joints have no limits')
      ok=pcall(function() Physics.ConnectRevolute(A,A,Vector2(0,0)) end); assert(not ok,'a joint needs two bodies')
      Actor.Destroy(Actor.Find('polygon'))
    )");
    Step(engine, 1);
    Lua(state, R"(
      assert(not W:IsValid() and not M:IsValid(),'destroying a body invalidates its joints')
      assert(R:IsValid(),'unrelated joints stay valid')
      -- character mover: push diagonally into the wall column at x=30
      local before=MOVER:GetPosition()
      local moved=Physics.MoveCharacter(MOVER,Vector2(3,1))
      local after=MOVER:GetPosition()
      assert(after.x<29.61 and after.x>29.5,'mover stops at the wall: '..after.x)
      assert(math.abs(after.y-before.y-1)<0.05,'mover slides along the wall: '..(after.y-before.y))
      assert(math.abs(moved.x-(after.x-before.x))<1e-4,'MoveCharacter returns the applied movement')
      ok=pcall(function() Physics.MoveCharacter(A,Vector2(1,0)) end); assert(not ok,'mover requires a kinematic body')
      MOVED_TO=after
      -- box mover: square corners, per-axis moves (tile platformers)
      local p0=BOXER:GetPosition()
      Physics.MoveCharacter(BOXER,Vector2(0,1))
      local p1=BOXER:GetPosition()
      assert(p1.y<=9.2+1e-3 and p1.y>=9.15,'box mover lands on the floor top: '..p1.y)
      local slid=Physics.MoveCharacter(BOXER,Vector2(3,0))
      assert(math.abs(slid.x-3)<1e-4 and math.abs(slid.y)<1e-4,'box mover walks along the floor')
      BOXER:SetPosition(Vector2(19.45,p1.y))
      Physics.MoveCharacter(BOXER,Vector2(0,1))
      assert(BOXER:GetPosition().y<9.21,'a box over the ledge edge does not slip off the corner')
      BOXER:SetPosition(Vector2(27,3))   -- clear of the circle mover resting at the wall
      Physics.MoveCharacter(BOXER,Vector2(5,0))
      local wall=BOXER:GetPosition().x
      assert(wall>29.5 and wall<29.61,'box mover stops at the wall: '..wall)
      -- grid path finding around solid tiles and static colliders
      local path=Physics.FindPath(Vector2(8,5),Vector2(12,5),0.5,0.2,4000)
      assert(#path>0 and path[#path].x==12 and path[#path].y==5,'path reaches the goal exactly')
      for _,p in ipairs(path) do
        assert(not (p.x>9.3 and p.x<10.7 and p.y>1.8 and p.y<8.2),'path avoids the static pillar')
      end
      local around=Physics.FindPath(Vector2(25,5),Vector2(35,5),1,0.2,20000)
      assert(#around>0 and around[#around].x==35,'path goes around the full-height tile wall')
      for _,p in ipairs(around) do assert(not (p.x>30 and p.x<31 and p.y>0 and p.y<12),'path never enters wall cells') end
      -- the clearance radius keeps a path off solid tiles as off colliders
      local wide=Physics.FindPath(Vector2(25,5),Vector2(35,5),0.25,0.4,200000)
      assert(#wide>0 and wide[#wide].x==35,'a fine path still goes around the wall')
      for _,p in ipairs(wide) do
        assert(not (p.x>29.61 and p.x<31.39 and p.y>-0.39 and p.y<12.39),'path keeps its clearance from tiles: '..p.x..','..p.y)
      end
      local partial=Physics.FindPath(Vector2(25,5),Vector2(35,5),1,0.2,10)
      assert(#partial>0 and partial[#partial].x~=35,'a node limit returns the best partial path')
      -- a walker closer to the pillar than its clearance can still leave, and reach a goal beside it
      local out=Physics.FindPath(Vector2(9.3,5),Vector2(6,5),0.5,1.0,4000)
      assert(#out>0 and out[#out].x==6 and out[#out].y==5,'path leaves the clearance margin around the start')
      local beside=Physics.FindPath(Vector2(6,5),Vector2(9.3,3),0.5,1.0,4000)
      assert(#beside>0 and math.abs(beside[#beside].x-9.3)<1e-5,'a goal inside the clearance margin is reached')
      local past=Physics.FindPath(Vector2(9.3,5),Vector2(11.5,5),0.5,1.0,20000)
      assert(#past>0 and math.abs(past[#past].x-11.5)<1e-5,'path from the margin reaches the far side')
      for _,p in ipairs(past) do
        assert(not (p.x>9.5 and p.x<10.5 and p.y>2 and p.y<8),'path never enters the pillar')
      end
      -- from inside the margin the path steps out at once instead of hugging the pillar's face
      local hug=Physics.FindPath(Vector2(9.2,5),Vector2(9.2,9.5),0.25,0.4,20000)
      assert(#hug>0 and math.abs(hug[#hug].y-9.5)<1e-5,'path from the margin reaches the goal past the pillar end')
      local close=0
      for i=1,#hug-1 do if hug[i].x>9.1 and hug[i].y<8.3 then close=close+1 end end
      assert(close<=1,'path leaves the clearance margin at once: '..close..' waypoints along the face')
      -- connected regions: one flood per region instead of a path per pair of points
      local r=Physics.PathRegions({Vector2(8,5),Vector2(12,5),Vector2(50,5),Vector2(56,5),Vector2(50.4,5.2)},1,0.2,20000)
      assert(#r==5 and r[1]==1 and r[2]==1,'points around the pillar share a region')
      assert(r[3]==2 and r[5]==2,'the walled pen is a region of its own')
      assert(r[4]==1,'outside the pen, past the tile wall, is the first region')
      local into=Physics.FindPath(Vector2(56,5),Vector2(50,5),1,0.2,20000)
      assert(#into==0 or into[#into].x~=50,'FindPath agrees: it cannot enter the pen')
      local cut=Physics.PathRegions({Vector2(8,5),Vector2(12,5)},0.5,0.2,3)
      assert(cut[1]==1 and cut[2]==2,'a node limit splits a region instead of joining')
      assert(#Physics.PathRegions({},1,0.2,10)==0,'no points, no labels')
      ok=pcall(function() Physics.PathRegions({1,2},1,0.2,10) end); assert(not ok,'points must be Vector2 values')
      ok=pcall(function() Physics.PathRegions({Vector2(0,0)},0,0.2,10) end); assert(not ok,'the cell size must be positive')
      -- slide along the 20-cell floor top
      SLIDER:SetVelocity(Vector2(8,0))
    )");
    Step(engine, 1);
    Lua(state, R"(
      -- kinematic bodies follow their Transform each step: the move must persist
      local now=MOVER:GetPosition()
      assert(math.abs(now.x-MOVED_TO.x)<1e-4 and math.abs(now.y-MOVED_TO.y)<1e-4,'mover keeps its position after a step')
      local t=MOVER.actor:GetComponent('Transform')
      assert(math.abs(t.x-MOVED_TO.x)<1e-4,'MoveCharacter writes the Transform')
    )");
    float slowest = 1e9f;
    for (int i = 0; i < 60; ++i) {
        Step(engine, 1);
        auto *slider = LuaBoundary::Cast<Rigidbody *>(ComponentManager::GetComponentByKey(7, "body"));
        if (slider->GetPosition().x < 18.0f) slowest = std::min(slowest, slider->GetVelocity().x);
    }
    Check(slowest > 7.5f, "sliding along tilemap cells lost speed at a seam: " + std::to_string(slowest));
    Lua(state, R"(
      Physics.SetGravity(Vector2(0,-1)); local g=Physics.GetGravity(); assert(g.y==-1,'gravity can be changed')
    )");
}
} // namespace

int main() {
    try {
        Run();
        std::cout << "physics_api_test: queries, layers, shapes, joints, mover and tilemap sliding passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "physics_api_test: " << error.what() << '\n';
        return 1;
    }
}
