#include "core/Engine.h"
#include "core/FrameClock.h"
#include "editor/documents/TilemapDocument.h"
#include "input/Input.h"
#include "physics/Rigidbody.h"
#include "scene/Tilemap.h"
#include "scene/Transform.h"
#include "physics/RayCast.h"
#include "scripting/ComponentManager.h"
#include "shared/resources/ResourcePath.h"
#include "lua.hpp"
#include "LuaBridge/LuaBridge.h"
#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <stdexcept>

namespace {
void Check(bool value, const std::string &message) {
    if (!value) throw std::runtime_error(message);
}
Tilemap *Map() { return ComponentManager::GetComponentByKey(10,"map").cast<Tilemap *>().value(); }
int Number(Actor::UID uid, const char *key, const char *field) {
    return ComponentManager::GetComponentByKey(uid,key)[field].cast<int>().value();
}
class Driver {
public:
    explicit Driver(Engine &engine) : engine_(engine) {
        Check(engine.GetLastSceneLoadError().empty(),engine.GetLastSceneLoadError());
        engine.InitializeHeadless();
        Check(FrameClock::GetFrameNumber()==0,"new runtime must start at frame zero");
    }
    void Step(int count, std::initializer_list<SDL_Scancode> keys={}) {
        std::set<SDL_Scancode> next(keys);
        auto emit=[](SDL_Scancode key,bool down) {
            SDL_Event event{};
            event.type=down?SDL_EVENT_KEY_DOWN:SDL_EVENT_KEY_UP;
            event.key.scancode=key;
            event.key.key=SDL_GetKeyFromScancode(key,SDL_KMOD_NONE,false);
            event.key.down=down;
            Input::ProcessEvent(event);
        };
        for(auto key:held_) if(!next.count(key)) emit(key,false);
        for(auto key:next) if(!held_.count(key)) emit(key,true);
        held_=std::move(next);
        const int before=FrameClock::GetFrameNumber();
        for(int i=0;i<count;++i) engine_.StepSimulation();
        Check(FrameClock::GetFrameNumber()==before+count,"step frame count mismatch");
        Check(ComponentManager::GetRuntimeErrorCount()==0,ComponentManager::GetLastRuntimeError());
    }
private:
    Engine &engine_;
    std::set<SDL_Scancode> held_;
};
void FarmState(int frame,int tile,int harvested,int planted,int mature) {
    Check(FrameClock::GetFrameNumber()==frame,"unexpected checkpoint frame");
    Check(Map()->GetCell("ground",8,8)==tile,"wrong committed crop tile");
    Check(Number(3,"logic","tick")==frame,"Lua farm clock differs from simulation");
    Check(Number(3,"logic","harvested")==harvested,"wrong harvest count");
    Check(Number(3,"logic","planted_count")==planted,"wrong live crop count");
    Check(Number(3,"logic","mature_count")==mature,"wrong mature crop count");
}
void TestFarmTimeline() {
    Engine engine; Driver driver(engine);
    Check(Map()->GetCell("ground",8,8)==2,"map must be queryable before first tick");
    Check(Map()->GetRevision()==0,"initial map revision must be zero");
    driver.Step(1,{SDL_SCANCODE_E});FarmState(1,3,0,1,0);
    Check(Number(3,"logic","mature_at_tick")==121,"initial maturity deadline mismatch");
    driver.Step(59,{SDL_SCANCODE_E});FarmState(60,3,0,1,0);
    driver.Step(1);driver.Step(1,{SDL_SCANCODE_E});driver.Step(1);FarmState(63,3,0,1,0);
    Check(Number(3,"logic","mature_at_tick")==121,"early interaction reset growth timer");
    driver.Step(57);FarmState(120,3,0,1,0);
    driver.Step(1);FarmState(121,4,0,1,1);
    driver.Step(1,{SDL_SCANCODE_E});FarmState(122,2,1,0,0);
    driver.Step(10,{SDL_SCANCODE_E});FarmState(132,2,1,0,0);
    driver.Step(1);driver.Step(1,{SDL_SCANCODE_E});FarmState(134,3,1,1,0);
    Check(Number(3,"logic","mature_at_tick")==254,"second crop deadline mismatch");
    driver.Step(40,{SDL_SCANCODE_RIGHT});driver.Step(1,{SDL_SCANCODE_E});FarmState(175,3,1,1,0);
    Check(Map()->GetCell("ground",12,8)==1,"nonfarmable ground must not change");
    Check(Map()->GetRevision()==4,"plant/mature/harvest/replant should commit four changes");
}
void PlacePlayer(float x,float y) {
    Check(ComponentManager::SetRuntimeTransformWorldPosition(2,x,y),"player positioning failed");
    // Dynamic roots are physics-owned: the transform helper changes the pose
    // cache, while teleporting the live Box2D body requires Rigidbody's API.
    auto *body=ComponentManager::GetComponentByKey(2,"body").cast<Rigidbody *>().value();
    body->SetPosition(Vector2(x,y));
    Check(body->GetPosition()==Vector2(x,y),"fixture did not teleport the real body");
}
float PlayerX() {
    return ComponentManager::GetComponentByKey(2,"body").cast<Rigidbody *>().value()->GetPosition().x;
}
void TestWallRemoval() {
    Engine engine;Driver driver(engine);driver.Step(1);
    PlacePlayer(7.5f,1.5f);
    driver.Step(30,{SDL_SCANCODE_RIGHT});
    Check(PlayerX()>7.5f && PlayerX()<=7.80f,"solid tile wall did not block the real body");
    driver.Step(1,{SDL_SCANCODE_SPACE});
    Check(Map()->GetCell("solid",16,9)==0,"tool did not remove the facing wall cell");
    Check(Map()->GetCell("solid",16,8)==5 && Map()->GetCell("solid",16,10)==5,
          "removing one cell modified neighboring wall cells");
    driver.Step(30,{SDL_SCANCODE_RIGHT});
    Check(PlayerX()>8.5f,"body cannot pass through the removed wall cell");
    Check(Number(3,"logic","walls_removed")==1,"wall operation count mismatch");
}
void TestCollisionCommitBoundary() {
    Engine engine;Driver driver(engine);driver.Step(1);
    Check(ComponentManager::SetRuntimeComponentPropertyValue(2,"collision_probe","enabled",true),
          "could not enable collision probe");
    PlacePlayer(7.5f,1.5f);
    for(int i=0;i<60 && Number(2,"collision_probe","requests")==0;++i)
        driver.Step(1,{SDL_SCANCODE_RIGHT});
    Check(Number(2,"collision_probe","requests")==1,"real wall contact callback never ran");
    Check(Number(2,"collision_probe","before")==5 && Number(2,"collision_probe","during")==5,
          "SetCell exposed uncommitted collision-callback changes");
    Check(Map()->GetCell("solid",16,9)==5,"collision callback changed current frame's committed map");
    Check(FrameClock::GetFrameNumber()==Number(2,"collision_probe","request_frame")+1,
          "callback frame observation is inconsistent");
    driver.Step(1,{SDL_SCANCODE_RIGHT});
    Check(Map()->GetCell("solid",16,9)==0,"queued collision edit did not commit next tick");
    Check(Map()->GetRevision()==1,"collision edit must commit exactly once");
}

bool HasOverride(const Engine &engine,const std::string &key,const std::string &field,
                 const Actor::ComponentPropertyValue &expected) {
    const auto *actor=engine.GetRuntimeActorByUID(10);
    Check(actor!=nullptr,"terrain actor disappeared");
    const auto *component=SceneFormat::FindComponentSpec(actor->component_specs,key);
    Check(component!=nullptr,"terrain component spec disappeared");
    for(const auto &property:component->overrides)
        if(property.name==field)return property.value==expected;
    return false;
}

void TestRuntimeMutationRejection() {
    for(const auto &change:std::vector<std::pair<std::string,Actor::ComponentPropertyValue>>{
        {"width",128},{"height",128},{"cell_size",2.0},{"revision",17},
        {"map_resource",std::string("missing.tilemap.json")}}) {
        Engine engine;Driver driver(engine);driver.Step(1);
        Check(Map()->SetCell("ground",8,8,4),"could not prepare mutated runtime map");driver.Step(1);
        const auto before_revision=Map()->GetRevision();
        bool accepted=false,threw=false;
        try { accepted=ComponentManager::SetRuntimeComponentPropertyValue(10,"map",change.first,change.second); }
        catch(const std::exception &){threw=true;}
        Check(!threw,"runtime map property rejection threw after component teardown: "+change.first);
        Check(!accepted,"runtime accepted readonly/invalid map property: "+change.first);
        Check(ComponentManager::GetTilemap(10,"map")!=nullptr,"rejected property removed the map instance");
        Check(Map()->GetCell("ground",8,8)==4 && Map()->GetRevision()==before_revision,
              "rejected property reset committed gameplay cells");
        Check(Map()->map_resource=="farm.tilemap.json","rejected resource changed runtime reference");
        Check(!HasOverride(engine,"map",change.first,change.second),"rejected map property polluted authoring spec");
        driver.Step(1);
    }
    Engine engine;Driver driver(engine);driver.Step(1);
    const auto transform=ComponentManager::GetComponentByKey(10,"transform");
    SceneFormat::SetComponentPropertyMutation rotation{10,"transform","rotation",90.0};
    Check(!engine.ApplySceneEditCommand(SceneFormat::SceneEditCommand::Single({rotation})),
          "rotated tilemap edit was accepted");
    const auto after=ComponentManager::GetComponentByKey(10,"transform");
    Check(!after.isNil() && after.cast<Transform *>().value()->rotation==0,
          "rejected rotation removed/replaced the valid transform");
    Check(!HasOverride(engine,"transform","rotation",90.0),"rejected rotation polluted scene spec");
    Check(engine.GetRuntimeActorByUID(10)->parent_uid==Actor::kInvalidUID,"map unexpectedly gained a parent");
    Check(!engine.SetRuntimeActorParentByUID(10,2),"map parenting was accepted");
    const auto map_ref=ComponentManager::GetComponentByKey(10,"map");
    luabridge::setGlobal(map_ref.state(),map_ref,"__tilemap_review_map");
    const int lua_result=luaL_dostring(map_ref.state(),R"(
        local map=__tilemap_review_map
        for field,value in pairs({width=128,height=128,cell_size=2,revision=99,map_resource="missing.tilemap.json"}) do
            local ok=pcall(function() map[field]=value end)
            assert(not ok,"readonly field accepted: "..field)
        end
        local ok=pcall(function() map.actor:GetComponent("Transform").rotation=90 end)
        assert(not ok,"rotated map through Lua")
        assert(map.actor:SetParent(Actor.Find("player"))==false,"parented map through Lua")
        assert(map.width==64 and map.height==64 and map.cell_size==1 and map.revision==0)
        __tilemap_review_map=nil
    )");
    Check(lua_result==LUA_OK,"Lua Tilemap registration did not enforce readonly/owner contracts");
    driver.Step(1);
    (void)transform; // Keep a pre-edit Lua reference alive across rejection.
}

void TestPreloadRejectionPreservesWorld() {
    struct BadProperty { const char *key; const char *field; Actor::ComponentPropertyValue value; };
    const std::vector<BadProperty> invalid={
        {"map","width",128}, {"map","enabled",std::string("yes")},
        {"map","map_resource",std::string("missing.tilemap.json")},
        {"transform","x",1e100}, {"transform","y",std::string("not numeric")},
        {"transform","rotation",90.0}
    };
    for(const auto &bad:invalid) {
        Engine engine;Driver driver(engine);driver.Step(1);
        Map()->SetCell("ground",8,8,4);driver.Step(1);
        const auto generation=engine.GetRuntimeActorByUID(10)->runtime_generation;
        const auto count=engine.GetActorCount();
        const auto bodies=RayCast::GetPhysicsWorld()->GetBodyCount();
        auto candidate=SceneFormat::LoadSceneAsset("farm");
        candidate.scene_name="rejected_map_candidate";
        auto &terrain=candidate.actors.front();
        auto *component=SceneFormat::FindComponentSpec(terrain.component_specs,bad.key);
        Check(component!=nullptr,"candidate fixture component missing");
        SceneFormat::UpsertComponentProperty(*component,bad.field,bad.value);
        bool accepted=false,threw=false;
        try {accepted=engine.LoadSceneAsset(candidate);}catch(const std::exception &){threw=true;}
        Check(!threw,"preload rejection threw after destroying the previous world: "+std::string(bad.field));
        Check(!accepted,"invalid map candidate was accepted: "+std::string(bad.field));
        Check(engine.GetActorCount()==count && engine.GetRuntimeActorByUID(10)!=nullptr &&
                  engine.GetRuntimeActorByUID(10)->runtime_generation==generation,
              "failed map scene load replaced old actor generations");
        Check(Map()->GetCell("ground",8,8)==4,"failed scene load lost old gameplay cell edits");
        Check(RayCast::GetPhysicsWorld()!=nullptr && RayCast::GetPhysicsWorld()->GetBodyCount()==bodies,
              "failed scene load destroyed old collision bodies");
        Check(engine.GetCurrentSceneName()=="farm","failed scene load changed active scene name");
        driver.Step(1);
    }
}

void TestPreloadAssetRangePreservesWorld(const std::filesystem::path &fixture) {
    const auto temporary=std::filesystem::temp_directory_path()/
        ("pocket-tilemap-range-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup {
        std::filesystem::path temporary,original;
        ~Cleanup(){ResourcePath::SetResourcesRootPath(original);std::error_code error;std::filesystem::remove_all(temporary,error);}
    } cleanup{temporary,ResourcePath::ResourcesRootPath()};
    std::filesystem::copy(fixture,temporary,std::filesystem::copy_options::recursive);
    ResourcePath::SetResourcesRootPath(temporary);
    std::ifstream source(temporary/"maps/farm.tilemap.json");
    const std::string original((std::istreambuf_iterator<char>(source)),{});
    struct RangeCase {const char *name;double cell_size;double origin_x;bool single_cell;};
    const std::vector<RangeCase> cases={
        {"extent_product",1e37,-8.0,false},
        {"origin_plus_extent",1e36,3e38,false},
        {"render_scale",1e38,-8.0,true}
    };
    for(const auto &item:cases) {
        // Write valid JSON with representable positive float inputs directly:
        // a production writer may itself reject the intentionally invalid range.
        rapidjson::Document json;json.Parse(original.c_str());
        Check(!json.HasParseError(),"range fixture JSON did not parse");
        json["cell_size"].SetDouble(item.cell_size);
        if(item.single_cell) {
            json["width"].SetInt(1);json["height"].SetInt(1);
            json["layers"]["ground"]["cells"].Clear();
            json["layers"]["ground"]["cells"].PushBack(1,json.GetAllocator());
            json["layers"]["solid"]["cells"].Clear();
            json["layers"]["solid"]["cells"].PushBack(0,json.GetAllocator());
        }
        rapidjson::StringBuffer buffer;rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
        json.Accept(writer);
        const std::string resource=std::string(item.name)+".tilemap.json";
        {std::ofstream invalid(temporary/"maps"/resource);invalid<<buffer.GetString();}
        Engine engine;Driver driver(engine);driver.Step(1);
        Check(Map()->SetCell("ground",8,8,4),"range test could not prepare nondefault old state");
        driver.Step(1);
        const auto generation=engine.GetRuntimeActorByUID(10)->runtime_generation;
        const auto actor_count=engine.GetActorCount();
        const auto body_count=RayCast::GetPhysicsWorld()->GetBodyCount();
        const auto revision=Map()->GetRevision();
        auto candidate=SceneFormat::LoadSceneAsset("farm");
        candidate.scene_name=item.name;
        auto &terrain=candidate.actors.front();
        auto *map=SceneFormat::FindComponentSpec(terrain.component_specs,"map");
        auto *transform=SceneFormat::FindComponentSpec(terrain.component_specs,"transform");
        Check(map!=nullptr && transform!=nullptr,"range fixture terrain components missing");
        SceneFormat::UpsertComponentProperty(*map,"map_resource",resource);
        SceneFormat::UpsertComponentProperty(*transform,"x",item.origin_x);
        bool accepted=false,threw=false;
        try {accepted=engine.LoadSceneAsset(candidate);}catch(const std::exception &){threw=true;}
        Check(!threw,std::string(item.name)+": range failure threw after old world teardown");
        Check(!accepted,std::string(item.name)+": nonrepresentable derived map range accepted");
        Check(engine.GetActorCount()==actor_count && engine.GetRuntimeActorByUID(10)!=nullptr &&
                  engine.GetRuntimeActorByUID(10)->runtime_generation==generation,
              std::string(item.name)+": failed preload replaced actor generations");
        Check(Map()->GetCell("ground",8,8)==4 && Map()->GetRevision()==revision &&
                  Map()->map_resource=="farm.tilemap.json",
              std::string(item.name)+": failed preload lost old map state");
        Check(RayCast::GetPhysicsWorld()!=nullptr && RayCast::GetPhysicsWorld()->GetBodyCount()==body_count,
              std::string(item.name)+": failed preload destroyed old bodies");
        Check(engine.GetCurrentSceneName()=="farm",std::string(item.name)+": failed preload changed scene identity");
        driver.Step(1);
    }
}

void TestDisableAndRemoveCollision() {
    Engine engine;Driver driver(engine);
    Check(Map()->WorldToCell(0.5,0.5)==Vector2(8,8),"frame-zero map origin used incomplete Transform binding");
    Check(Map()->GetCellCenter(8,8)==Vector2(0.5f,0.5f),"frame-zero cell center is wrong");
    driver.Step(1);Map()->SetCell("ground",8,8,4);driver.Step(1);
    const auto revision=Map()->GetRevision();
    Check(ComponentManager::SetRuntimeComponentPropertyValue(10,"map","enabled",false),"map disable failed");
    driver.Step(1);
    Check(Map()->GetCell("ground",8,8)==4 && Map()->GetRevision()==revision,
          "disabling map reloaded authoring cells over gameplay state");
    PlacePlayer(7.5f,1.5f);driver.Step(30,{SDL_SCANCODE_RIGHT});
    Check(PlayerX()>8.5f,"disabled map left active wall collision");
    Check(ComponentManager::SetRuntimeComponentPropertyValue(10,"map","enabled",true),"map enable failed");
    driver.Step(1);PlacePlayer(7.5f,1.5f);driver.Step(30,{SDL_SCANCODE_RIGHT});
    Check(PlayerX()>7.5f && PlayerX()<=7.80f,"re-enabled map lost wall collision");
    auto expired_map=ComponentManager::GetComponentByKey(10,"map");
    Check(ComponentManager::SetRuntimeComponentPropertyValue(3,"logic","enabled",false),"could not stop fixture logic before deleting its map");
    Check(engine.DeleteRuntimeActorByUID(10),"map actor removal failed");
    Check(RayCast::GetPhysicsWorld()->GetBodyCount()==1,"deleting map leaked chunk bodies");
    driver.Step(30,{SDL_SCANCODE_RIGHT});
    Check(PlayerX()>8.5f,"deleted map left active wall collision");
    Check(!expired_map.cast<Tilemap *>().value()->SetCell("solid",16,9,5),"expired map accepted a mutation");
    expired_map.cast<Tilemap *>().value()->OnStart();expired_map.cast<Tilemap *>().value()->CommitPending();
    Check(RayCast::GetPhysicsWorld()->GetBodyCount()==1,"expired map resurrected collision bodies");
}

void TestRejectedPreloadDoesNotPoisonRebuild(const std::filesystem::path &fixture) {
    const auto temporary=std::filesystem::temp_directory_path()/
        ("pocket-tilemap-prepared-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup {
        std::filesystem::path temporary,original;
        ~Cleanup(){ResourcePath::SetResourcesRootPath(original);std::error_code error;std::filesystem::remove_all(temporary,error);}
    } cleanup{temporary,ResourcePath::ResourcesRootPath()};
    std::filesystem::copy(fixture,temporary,std::filesystem::copy_options::recursive);
    ResourcePath::SetResourcesRootPath(temporary);
    auto different=TilemapAsset::LoadResource("farm.tilemap.json");
    different.ground.cells[0]=8;
    TilemapAsset::SaveResource("different.tilemap.json",different);
    Engine engine;Driver driver(engine);driver.Step(1);
    const auto original_spec=*SceneFormat::FindComponentSpec(engine.GetRuntimeActorByUID(10)->component_specs,"map");
    auto candidate=SceneFormat::LoadSceneAsset("farm");
    auto *candidate_map=SceneFormat::FindComponentSpec(candidate.actors.front().component_specs,"map");
    SceneFormat::UpsertComponentProperty(*candidate_map,"map_resource",std::string("different.tilemap.json"));
    // This fails after valid map assets have been prepared, at component-type validation.
    candidate.actors.back().component_specs.push_back({"unknown","NonexistentTilemapReviewComponent",{}});
    Check(!engine.LoadSceneAsset(candidate),"unknown component candidate was accepted");
    Check(Map()->GetCell("ground",0,0)==1,"rejected preload immediately replaced old map");
    SceneFormat::DeleteComponentMutation remove{10,"map"};
    Check(engine.ApplySceneEditCommand(SceneFormat::SceneEditCommand::Single({remove})),"could not remove map before legitimate rebuild");
    SceneFormat::AddComponentMutation add{10,original_spec};
    Check(engine.ApplySceneEditCommand(SceneFormat::SceneEditCommand::Single({add})),"could not rebuild the original map component");
    Check(Map()->map_resource=="farm.tilemap.json" && Map()->GetCell("ground",0,0)==1,
          "failed scene's prepared asset poisoned later same-UID/key component rebuild");
}

void TestFirstTransformOriginBeforeStart() {
    Engine engine;Driver driver(engine);
    auto candidate=SceneFormat::LoadSceneAsset("farm");
    auto &terrain=candidate.actors.front();
    auto *primary=SceneFormat::FindComponentSpec(terrain.component_specs,"transform");
    Check(primary!=nullptr,"fixture primary Transform missing");
    primary->key="z_primary"; // Both Transform instances bind after the map key.
    auto secondary=*primary;
    secondary.key="zz_secondary";
    SceneFormat::UpsertComponentProperty(secondary,"x",40.0);
    SceneFormat::UpsertComponentProperty(secondary,"y",40.0);
    terrain.component_specs.push_back(std::move(secondary));
    Check(engine.LoadSceneAsset(candidate),engine.GetLastSceneLoadError());
    Check(FrameClock::GetFrameNumber()==0,"scene assembly advanced gameplay");
    Check(Map()->WorldToCell(0.5,0.5)==Vector2(8,8) &&
              Map()->GetCellCenter(8,8)==Vector2(0.5f,0.5f),
          "frame-zero fallback used the last Transform instead of the primary Transform");
    driver.Step(1,{SDL_SCANCODE_E});
    Check(Map()->WorldToCell(0.5,0.5)==Vector2(8,8) && Map()->GetCell("ground",8,8)==3,
          "first tick changed origin or planted in a different cell after binding");
}
void TestAuthoringDocument(const std::filesystem::path &fixture) {
    const auto temporary=std::filesystem::temp_directory_path()/
        ("pocket-tilemap-document-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup {
        std::filesystem::path temporary, original;
        ~Cleanup(){ResourcePath::SetResourcesRootPath(original);std::error_code error;std::filesystem::remove_all(temporary,error);}
    } cleanup{temporary,ResourcePath::ResourcesRootPath()};
    std::filesystem::create_directories(temporary/"maps");
    std::filesystem::copy(fixture/"images",temporary/"images",std::filesystem::copy_options::recursive);
    const auto first=temporary/"maps/first.tilemap.json", second=temporary/"maps/second.tilemap.json";
    std::filesystem::copy_file(fixture/"maps/farm.tilemap.json",first);
    std::filesystem::copy_file(first,second);
    ResourcePath::SetResourcesRootPath(temporary);
    TilemapDocument document;
    Check(document.Open(first),document.GetLastError());
    const auto initial_revision=document.GetRevision();
    Check(!document.HasUnsavedChanges(),"newly opened map is dirty");
    Check(document.SetCell("ground",8,8,3),"authoring edit failed");
    Check(document.HasUnsavedChanges(),"authoring edit was not tracked");
    Check(!document.Open(second) && document.GetPath()==first,"dirty map silently replaced");
    Check(document.Undo() && !document.HasUnsavedChanges(),"undo did not restore saved contents");
    Check(document.Redo() && document.HasUnsavedChanges(),"redo did not restore edit");
    Check(document.Save(),document.GetLastError());
    Check(TilemapAsset::LoadFile(first,temporary/"images").ground.cells[8*64+8]==3,"saved cell not restored");
    Check(document.Undo() && document.HasUnsavedChanges(),"undo after save must become dirty");
    Check(document.Redo() && !document.HasUnsavedChanges(),"redo to saved contents must become clean");
    Check(!document.SetCell("ground",-1,0,3),"negative authoring coordinate accepted");
    Check(!document.SetCell("ground",0,0,99),"invalid authoring tile accepted");
    Check(document.SetCell("ground",0,0,4),"second edit failed");
    Check(document.Revert() && !document.HasUnsavedChanges(),"explicit discard did not reload saved map");
    Check(document.Open(second),document.GetLastError());
    Check(!document.Open(temporary/"maps/missing.tilemap.json") && document.GetPath()==second,
          "failed open destroyed current document");
    std::ifstream saved_file(second);
    const std::string saved_text((std::istreambuf_iterator<char>(saved_file)),{});
    saved_file.close();
    Check(document.SetCell("ground",8,8,4),"edit before failed reload failed");
    { std::ofstream damaged(second); damaged<<"{broken"; }
    Check(!document.Revert() && document.HasUnsavedChanges() &&
              document.GetAsset().ground.cells[8*64+8]==4,
          "failed disk reload discarded unsaved authoring data");
    const auto before_discard=document.GetRevision();
    Check(document.DiscardChanges() && !document.HasUnsavedChanges() &&
              document.GetAsset().ground.cells[8*64+8]==2 &&
              !document.CanUndo() && !document.CanRedo() && document.GetRevision()>before_discard,
          "memory discard did not restore the saved snapshot and clear history");
    { std::ifstream damaged(second);std::string text((std::istreambuf_iterator<char>(damaged)),{});
      Check(text=="{broken","memory discard wrote to the damaged source file"); }
    { std::ofstream restored(second); restored<<saved_text; }
    Check(document.Revert() && !document.HasUnsavedChanges(),"restored disk asset could not reload");
    Check(document.SetCell("ground",0,0,8),"edit before deleted-source discard failed");
    Check(std::filesystem::remove(second),"could not prepare deleted map source");
    Check(!document.Revert() && document.HasUnsavedChanges(),"missing source silently discarded draft");
    Check(document.DiscardChanges() && !document.HasUnsavedChanges() &&
              document.GetAsset().ground.cells[0]==1 && !std::filesystem::exists(second),
          "discard must work without recreating a deleted source file");
    Check(document.GetRevision()>initial_revision,"preview revision did not advance");
    const auto last_revision=document.GetRevision();document.Clear();
    Check(!document.HasAsset() && document.GetRevision()>last_revision,"Clear must signal preview invalidation");
}
}

int main(int argc,char **argv) {
    try {
        Check(argc==2 || argc==3,"usage: tilemap_runtime_test FIXTURE_PROJECT [mutations|preload|disable|prepared|origin]");
        const auto fixture=std::filesystem::absolute(argv[1]);
        ResourcePath::SetResourcesRootPath(fixture);
        if(argc==3) {
            const std::string selected=argv[2];
            if(selected=="mutations")TestRuntimeMutationRejection();
            else if(selected=="preload"){TestPreloadRejectionPreservesWorld();TestPreloadAssetRangePreservesWorld(fixture);}
            else if(selected=="disable")TestDisableAndRemoveCollision();
            else if(selected=="prepared")TestRejectedPreloadDoesNotPoisonRebuild(fixture);
            else if(selected=="origin")TestFirstTransformOriginBeforeStart();
            else throw std::runtime_error("unknown test selector");
        } else {
            TestFarmTimeline();TestWallRemoval();TestCollisionCommitBoundary();TestAuthoringDocument(fixture);
            TestRuntimeMutationRejection();TestPreloadRejectionPreservesWorld();TestDisableAndRemoveCollision();
            TestPreloadAssetRangePreservesWorld(fixture);
            TestRejectedPreloadDoesNotPoisonRebuild(fixture);
            TestFirstTransformOriginBeforeStart();
        }
        std::cout<<"tilemap_runtime_test: "<<(argc==3?argv[2]:"all runtime/editor regressions")<<" passed\n";
        return 0;
    } catch(const std::exception &error) {
        std::cerr<<"tilemap_runtime_test: "<<error.what()<<'\n';return 1;
    }
}
