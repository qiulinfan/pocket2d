#include "core/Engine.h"
#include "input/Input.h"
#include "physics/Rigidbody.h"
#include "rendering/SpriteAnimation.h"
#include "rendering/SpriteRenderer.h"
#include "scene/Tilemap.h"
#include "scene/Transform.h"
#include "scripting/ComponentManager.h"
#include "scripting/ScriptAPIContract.h"
#include "shared/resources/ResourcePath.h"
#include "shared/tilemap/TilemapAsset.h"
#include "rapidjson/document.h"
#include "SDL3/SDL.h"
#include "lua.hpp"
#include "LuaBridge/LuaBridge.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>

namespace {
void Check(bool condition, const std::string &message) { if (!condition) throw std::runtime_error(message); }
void Lua(lua_State *state, const char *source) {
    const int top=lua_gettop(state);
    const int status=luaL_dostring(state,source);
    const std::string error=status==LUA_OK ? "" : lua_tostring(state,-1);
    lua_settop(state,top);
    Check(status==LUA_OK,error);
}
struct Fixture {
    const std::filesystem::path previous_root=ResourcePath::ResourcesRootPath();
    const std::filesystem::path root=std::filesystem::temp_directory_path()/
        ("pocket-script-api-contract-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    bool had_save_root=false;
    std::string previous_save_root;
    Fixture() {
        if (const char *value=SDL_getenv("POCKET_SAVE_ROOT")) { had_save_root=true; previous_save_root=value; }
        for (const char *directory:{"images","maps","animations","items","actor_templates","scenes","component_types"})
            std::filesystem::create_directories(root/"project"/directory);
        ResourcePath::SetResourcesRootPath(root/"project");
        SDL_setenv_unsafe("POCKET_SAVE_ROOT",(root/"saves").string().c_str(),1);
        auto *atlas=SDL_CreateSurface(32,16,SDL_PIXELFORMAT_RGBA32);
        Check(atlas!=nullptr,"create contract atlas");
        SDL_FillSurfaceRect(atlas,nullptr,SDL_MapSurfaceRGBA(atlas,150,80,200,255));
        const bool saved=SDL_SaveBMP(atlas,(root/"project/images/atlas.bmp").string().c_str());
        SDL_DestroySurface(atlas); Check(saved,"save contract atlas");
        TilemapAsset::Data map;
        map.width=map.height=2; map.atlas={"atlas.bmp",2,1,16,16};
        map.ground.cells={1,1,1,1}; map.solid.cells={0,0,0,0};
        TilemapAsset::ValidateAtlasImage(map,root/"project/images");
        TilemapAsset::SaveResource("contract.tilemap.json",map);
        std::ofstream(root/"project/animations/contract.animation.json") << R"({"version":1,"image":"atlas.bmp","columns":2,"rows":1,"tile_width_px":16,"tile_height_px":16,"clips":[{"name":"walk","loop":true,"frames":[{"row":1,"column":1,"ticks":2},{"row":1,"column":2,"ticks":2}]},{"name":"attack","loop":false,"frames":[{"row":1,"column":1,"ticks":1,"event":"start"},{"row":1,"column":2,"ticks":1,"event":"hit"}]}]})";
        std::ofstream(root/"project/game.config") << R"({"initial_scene":"contract","save_namespace":"ScriptApiContract"})";
        std::ofstream(root/"project/items/contract.items.json") << R"({"version":1,"items":[
          {"id":"wood","display_name":"Wood","kind":"material","max_stack":10},
          {"id":"food","display_name":"Food","kind":"food","max_stack":5,"health_restore":3},
          {"id":"sword","display_name":"Sword","kind":"weapon","max_stack":1,"equipment_slot":"subweapon","attack":7},
          {"id":"hat","display_name":"Hat","kind":"armor","max_stack":1,"equipment_slot":"head","defense":4,"move_speed_bonus":0.5}
        ]})";
        std::ofstream(root/"project/actor_templates/Contract.template") << R"({"name":"spawned","components":{"transform":{"type":"Transform"}}})";
        std::ofstream(root/"project/scenes/contract.scene") << R"({"actors":[
          {"uid":1,"name":"player","components":{
            "body":{"type":"Rigidbody","gravity_scale":0,"has_collider":false,"has_trigger":false},
            "transform":{"type":"Transform"},"sprite":{"type":"SpriteRenderer","sprite":"atlas.bmp"},
            "animation":{"type":"SpriteAnimation","animation_resource":"contract.animation.json","clip":"walk"},
            "probe":{"type":"ContractProbe"}}},
          {"uid":2,"name":"terrain","components":{"map":{"type":"Tilemap","map_resource":"contract.tilemap.json"},
            "transform":{"type":"Transform","x":10,"y":20}}},
          {"uid":3,"name":"child","parent_uid":1,"components":{"transform":{"type":"Transform","x":1}}}
        ]})";
        std::ofstream(root/"project/component_types/ContractProbe.lua") << R"(
ContractProbe={events=0,completions=0,key_held=false,key_down=false,key_up=false,
  OnUpdate=function(self)
    self.key_held=Input.GetKey("right")
    self.key_down=Input.GetKeyDown("right")
    self.key_up=Input.GetKeyUp("right")
  end,
  OnAnimationEvent=function(self,event)
    assert(self.actor:IsValid() and self.key=="probe")
    self.events=self.events+1
    if event.type=="completed" then self.completions=self.completions+1 end
    self.last_event=event
  end}
)";
    }
    ~Fixture() {
        ResourcePath::SetResourcesRootPath(previous_root);
        if(had_save_root) SDL_setenv_unsafe("POCKET_SAVE_ROOT",previous_save_root.c_str(),1);
        else SDL_unsetenv_unsafe("POCKET_SAVE_ROOT");
        std::error_code error; std::filesystem::remove_all(root,error);
    }
};
const rapidjson::Value *NamedType(const rapidjson::Value &contract,const std::string &name) {
    for(const auto &type:contract["types"].GetArray()) if(type["name"].GetString()==name) return &type;
    return nullptr;
}
bool NativeType(const luabridge::LuaRef &value,const std::string &type) {
    if(type=="Actor") return value.isInstance<ActorHandle>();
    if(type=="Vector2") return value.isInstance<b2Vec2>();
    if(type=="Transform") return value.isInstance<Transform>();
    if(type=="SpriteRenderer") return value.isInstance<SpriteRenderer>();
    if(type=="Rigidbody") return value.isInstance<Rigidbody>();
    if(type=="Tilemap") return value.isInstance<Tilemap>();
    if(type=="SpriteAnimation") return value.isInstance<SpriteAnimation>();
    if(type=="ItemCatalog" || type=="InventoryContainer") {
        auto *state=value.state(); value.push(state);
        const bool matches=luaL_testudata(state,-1,type=="ItemCatalog"?"Pocket2D.ItemCatalog":"Pocket2D.Inventory")!=nullptr;
        lua_pop(state,1); return matches;
    }
    throw std::runtime_error("new native contract type needs a real fixture: "+type);
}
bool Matches(const luabridge::LuaRef &value,const rapidjson::Value &type,const rapidjson::Value &contract,int depth=0) {
    if(depth>32) return false;
    if(type.IsObject()) {
        if(type.HasMember("nullable")) return value.isNil() || Matches(value,type["nullable"],contract,depth+1);
        if(type.HasMember("array")) {
            if(!value.isTable()) return false;
            for(int i=1;i<=value.length();++i) if(!Matches(value[i],type["array"],contract,depth+1)) return false;
            return true;
        }
        if(type.HasMember("union")) {
            for(const auto &branch:type["union"].GetArray()) if(Matches(value,branch,contract,depth+1)) return true;
            return false;
        }
        if(type.HasMember("literal")) {
            const auto &literal=type["literal"];
            if(literal.IsString()) return value.isString() && value.cast<std::string>().value()==literal.GetString();
            if(literal.IsBool()) return value.isBool() && value.cast<bool>().value()==literal.GetBool();
            return false;
        }
        throw std::runtime_error("unhandled structured contract type");
    }
    const std::string name=type.GetString();
    if(name=="unknown") return true;
    if(name=="void") return value.isNil();
    if(name=="boolean") return value.isBool();
    if(name=="string") return value.isString();
    if(name=="number") return value.isNumber();
    if(name=="integer") {
        auto *state=value.state(); value.push(state); const bool integer=lua_isinteger(state,-1); lua_pop(state,1); return integer;
    }
    const auto *definition=NamedType(contract,name); Check(definition!=nullptr,"unknown contract type "+name);
    const std::string kind=(*definition)["kind"].GetString();
    if(kind=="native" || kind=="component") return NativeType(value,name);
    if(kind=="alias") return Matches(value,(*definition)["type"],contract,depth+1);
    if(kind=="record") {
        if(!value.isTable() && !value.isUserdata()) return false;
        for(const auto &property:(*definition)["properties"].GetArray())
            if(!Matches(value[property["name"].GetString()],property["type"],contract,depth+1)) return false;
        return true;
    }
    if(kind=="dictionary") return value.isTable(); // SaveStore's recursive validation has its own tests.
    throw std::runtime_error("unhandled contract kind "+kind);
}
int Assign(lua_State *state) { lua_settop(state,3); lua_settable(state,1); return 0; }
bool TryAssign(const luabridge::LuaRef &object,const char *name,const luabridge::LuaRef &value) {
    auto *state=object.state(); const int top=lua_gettop(state);
    lua_pushcfunction(state,Assign); object.push(state); lua_pushstring(state,name); value.push(state);
    const bool ok=lua_pcall(state,3,0,0)==LUA_OK;
    lua_settop(state,top); return ok;
}
struct Coverage { int properties=0,methods=0,statics=0,constructors=0; };
void Members(const luabridge::LuaRef &instance,const rapidjson::Value &definition,
             const rapidjson::Value &contract,Coverage &coverage) {
    if(definition.HasMember("extends")) {
        const auto *base=NamedType(contract,definition["extends"].GetString()); Check(base!=nullptr,"missing component base");
        Members(instance,*base,contract,coverage);
    }
    for(const auto &property:definition["properties"].GetArray()) {
        const char *name=property["name"].GetString(); const auto value=luabridge::LuaRef(instance[name]);
        const std::string location=std::string(definition["name"].GetString())+"."+name;
        Check(Matches(value,property["type"],contract),location+" runtime property type differs from SDK");
        const std::string access=property["access"].GetString();
        const bool wrote=TryAssign(instance,name,value);
        if(access=="readonly") Check(!wrote,location+" is advertised runtime readonly but accepted assignment");
        else {
            Check(access=="mutable" || access=="engine_owned","unknown property access contract");
            Check(wrote,location+" valid same-value write unexpectedly rejected");
        }
        ++coverage.properties;
    }
    for(const auto &method:definition["methods"].GetArray()) {
        const std::string location=std::string(definition["name"].GetString())+"."+method["name"].GetString();
        Check(std::string(method["call"].GetString())=="instance",location+" wrong self convention");
        Check(instance[method["name"].GetString()].isFunction(),location+" missing native method");
        ++coverage.methods;
    }
}
void VerifyManifest(lua_State *state,const rapidjson::Value &contract,Coverage &coverage) {
    Check(contract["contract_version"].GetInt()==1 && std::string(contract["coverage"].GetString())=="curated","unexpected contract version/coverage");
    Lua(state,R"(
      local catalog=Inventory.Open("contract.items.json")
      local inventory=catalog:Create(4)
      contract_instances={Actor=Actor.Find("player"),Vector2=Vector2(3,4),
        Transform=Actor.Find("player"):GetComponent("Transform"),
        SpriteRenderer=Actor.Find("player"):GetComponent("SpriteRenderer"),
        Rigidbody=Actor.Find("player"):GetComponent("Rigidbody"),
        Tilemap=Actor.Find("terrain"):GetComponent("Tilemap"),
        SpriteAnimation=Actor.Find("player"):GetComponent("SpriteAnimation"),
        ItemCatalog=catalog,InventoryContainer=inventory}
      contract_records={ItemDefinition=catalog:GetItem("wood"),
        InventorySlot=inventory:GetSlot(1),InventoryResult=inventory:Add("wood",5),
        InventoryRestoreEntry={inventory=inventory,slots=inventory:Snapshot()}}
    )");
    const auto instances=luabridge::getGlobal(state,"contract_instances");
    for(const auto &definition:contract["types"].GetArray()) {
        const std::string kind=definition["kind"].GetString();
        if(kind!="native" && kind!="component") continue;
        const std::string name=definition["name"].GetString();
        const auto instance=luabridge::LuaRef(instances[name]);
        Check(!instance.isNil() && NativeType(instance,name),"missing runtime fixture for "+name);
        // nominal markers live only in the generated types, never in the list
        // of runtime properties. Records/aliases also are not native globals.
        Members(instance,definition,contract,coverage);
    }
    const auto records=luabridge::getGlobal(state,"contract_records");
    for(const auto *name:{"ItemDefinition","InventorySlot","InventoryResult","InventoryRestoreEntry"}) {
        const auto *definition=NamedType(contract,name); Check(definition!=nullptr,"missing inventory record type");
        Members(records[name],*definition,contract,coverage);
    }
    for(const auto &space:contract["namespaces"].GetArray()) {
        const auto object=luabridge::getGlobal(state,space["name"].GetString());
        Check(object.isTable(),"missing native namespace");
        for(const auto &function:space["functions"].GetArray()) {
            Check(std::string(function["call"].GetString())=="static","namespace requires static/noSelf convention");
            Check(object[function["name"].GetString()].isFunction(),"missing advertised static function");
            ++coverage.statics;
        }
    }
    for(const auto &constructor:contract["constructors"].GetArray()) {
        Check(std::string(constructor["call"].GetString())=="constructor" &&
              std::string(constructor["name"].GetString())=="Vector2","new constructor needs a real call probe");
        Lua(state,"assert(type(Vector2(1,2))=='userdata')"); ++coverage.constructors;
    }
}
void EmitKey(bool down) {
    SDL_Event event{}; event.type=down?SDL_EVENT_KEY_DOWN:SDL_EVENT_KEY_UP;
    event.key.scancode=SDL_SCANCODE_RIGHT; event.key.key=SDLK_RIGHT; event.key.down=down;
    Input::ProcessEvent(event);
}
void Calls(Engine &engine,lua_State *state,const rapidjson::Value &contract) {
    engine.StepSimulation(); // Real native OnStart, body creation and first animation event.
    Lua(state,R"(
      local function near(a,b) assert(math.abs(a-b)<0.001,tostring(a).." != "..tostring(b)) end
      local function rejects(f) local ok=pcall(f);assert(not ok,"invalid call unexpectedly succeeded") end
      local player=Actor.Find("player");assert(player~=nil and player:IsValid() and player.is_valid and player:GetName()=="player")
      assert(Actor.Find("absent")==nil and #Actor.FindAll("absent")==0)
      assert(#Actor.FindAll("player")==1 and Actor.FindAll("player")[1]==player)
      assert(player:GetComponent("Missing")==nil and player:GetComponentByKey("missing")==nil)
      assert(player:GetComponentInChildren("Missing")==nil and #player:GetComponents("Missing")==0)
      assert(player:GetChildCount()==1 and #player:GetChildren()==1)
      local child=player:GetChildren()[1];assert(child:GetName()=="child")
      assert(#player:GetComponentsInChildren("Transform")==2 and #player:GetComponents("Transform")==1)
      assert(player:GetComponentInChildren("Transform")==player:GetComponent("Transform"))
      assert(child:SetParent(nil) and player:GetChildCount()==0)
      assert(child:SetParent(player) and player:GetChildCount()==1)
      local extra=child:AddComponent("SpriteRenderer");assert(extra~=nil and extra.actor==child)
      local key=extra.key;assert(child:GetComponentByKey(key)==extra)
      child:RemoveComponent(extra);assert(child:GetComponentByKey(key)==nil)
      local spawned=Actor.Instantiate("Contract");assert(spawned~=nil and spawned:IsValid())
      Actor.Destroy(spawned);assert(not spawned:IsValid() and not spawned.is_valid)
      assert(spawned:GetComponentByKey("transform")==nil and #spawned:GetChildren()==0)
      rejects(function() Actor.Destroy({}) end)
      local v=Vector2(3,4);near(v:Length(),5);near(v:Normalize(),5);near(v:Length(),1)
      v.x=6;v.y=8;near(Vector2.Distance(v,Vector2(0,0)),10);near(Vector2.Dot(v,Vector2(1,0)),6)
      rejects(function() Vector2("not a number",0) end)
      rejects(function() Vector2.Distance({x=0,y=0},v) end)
      local body=player:GetComponent("Rigidbody")
      assert(body:SetPosition(Vector2(1,2))==nil);near(body:GetPosition().x,1);near(body:GetPosition().y,2)
      body:SetVelocity(Vector2(3,0));near(body:GetVelocity().x,3);body:AddForce(Vector2(0,0))
      body:SetRotation(90);near(body:GetRotation(),90)
      body:SetUpDirection(Vector2(0,-1));near(body:GetUpDirection().y,-1)
      body:SetRightDirection(Vector2(1,0));near(body:GetRightDirection().x,1)
      body:SetAngularVelocity(30);near(body:GetAngularVelocity(),30);body:SetAngularVelocity(0)
      body:SetGravityScale(2);near(body:GetGravityScale(),2);body:SetGravityScale(0)
      rejects(function() body:SetVelocity({x=3,y=0}) end)
      rejects(function() body:SetVelocity(nil) end)
      rejects(function() body.SetVelocity(Vector2(1,0)) end) -- missing instance self
      rejects(function() body.x="invalid number" end)
      local transform=child:GetComponent("Transform");transform.x=2;transform.y=3;transform.rotation=15
      near(transform.x,2);near(transform.y,3);near(transform.rotation,15)
      local sprite=player:GetComponent("SpriteRenderer");sprite:SetSpriteCell(1,2)
      assert(sprite.sprite_row==1 and sprite.sprite_column==2);sprite.r=128;sprite.a=200;sprite.scale_x=-1
      rejects(function() sprite.not_a_member=1 end)
      local map=Actor.Find("terrain"):GetComponent("Tilemap")
      assert(map.width==2 and map.height==2 and map:InBounds(1,1) and not map:InBounds(-1,0))
      local cell=map:WorldToCell(10.5,20.5);near(cell.x,0);near(cell.y,0)
      local center=map:GetCellCenter(1,1);near(center.x,11.5);near(center.y,21.5)
      assert(map:GetCell("ground",0,0)==1 and map:GetCell("ground",99,0)==nil)
      assert(map:GetRevision()==0 and map.revision==0)
      assert(map:SetCell("ground",0,0,2) and map:GetCell("ground",0,0)==1)
      assert(not map:SetCell("ground",99,0,2))
      rejects(function() map:GetCell("other",0,0) end)
      rejects(function() map:GetCell("ground",0.5,0) end)
      local animation=player:GetComponent("SpriteAnimation")
      assert(not animation:Play("walk"))
      assert(animation:Pause()==nil and animation.paused and not animation.playing)
      assert(animation:Resume()==nil and not animation.paused and animation.playing)
      assert(animation:Stop()==nil and animation.frame==0 and animation.current_clip=="")
      assert(animation:Play("walk") and animation.frame==1)
      rejects(function() animation:Play("missing") end)
      assert(animation.current_clip=="walk")
      assert(animation:Restart("attack") and animation.frame==1 and animation.elapsed_ticks==0)
      assert(not Input.GetKey("right") and not Input.GetKeyDown("right") and not Input.GetKeyUp("right"))
      contract_write=Save.Write("api",2,{health=7,values={1,2},flag=true})
      assert(contract_write.ok)
      contract_read=Save.Read("api");assert(contract_read.ok and contract_read.schema_version==2)
      assert(type(contract_read.data)=="table" and contract_read.data.health==7 and contract_read.data.values[2]==2)
      contract_failure=Save.Read("missing");assert(not contract_failure.ok and contract_failure.code=="not_found")
      assert(not Save.Read({}).ok and Save.Read({}).code=="invalid_argument")
      assert(not Save.Write("api",1.5,{}).ok and not Save.Write("api",1,Vector2(1,2)).ok)
      local catalog=Inventory.Open("contract.items.json")
      assert(type(catalog)=="userdata" and catalog:GetItem("absent")==nil)
      local definition=catalog:GetItem("wood");assert(definition.id=="wood" and definition.max_stack==10)
      definition.max_stack=999;assert(catalog:GetItem("wood").max_stack==10)
      local bag=catalog:Create(4);local chest=catalog:Create(3,nil)
      local equipment=catalog:Create(1,{"subweapon"})
      assert(bag:GetCapacity()==4 and bag:GetRevision()==0)
      assert(#bag:Snapshot()==4 and bag:GetSlot(1).count==0 and bag:GetSlot(1).item_id=="")
      local added=bag:Add("wood",8);assert(added.ok and added.changed and added.moved==8 and added.remaining==0)
      assert(bag:GetRevision()==1)
      local copy=bag:GetSlot(1);copy.count=999;copy.item_id="food";assert(bag:GetSlot(1).count==8 and bag:GetSlot(1).item_id=="wood")
      local copied=bag:Snapshot();copied[1].count=222;assert(bag:GetSlot(1).count==8)
      assert(bag:Split(1,chest,1,3).ok and bag:GetSlot(1).count==5 and chest:GetSlot(1).count==3)
      assert(bag:Move(1,chest,1,2,false).ok and chest:GetSlot(1).count==5)
      assert(bag:Move(1,chest,2).ok and bag:GetSlot(1).count==0 and chest:GetSlot(2).count==3)
      assert(chest:TransferAll(2,bag).ok and bag:GetSlot(1).count==3)
      assert(bag:Add("food",2,nil).ok and bag:Remove(1,1).ok)
      assert(bag:Add("sword",1,"sword-instance").ok)
      assert(bag:Move(3,equipment,1,nil,false).ok and equipment:GetSlot(1).instance_id=="sword-instance")
      local before=bag:Snapshot();local revision=bag:GetRevision()
      contract_inventory_failure=bag:Move(1,Inventory.Open("contract.items.json"):Create(1),1)
      assert(not contract_inventory_failure.ok and contract_inventory_failure.code=="incompatible_catalog" and bag:GetRevision()==revision)
      rejects(function() bag:GetSlot(0) end)
      rejects(function() bag:GetSlot(5) end)
      rejects(function() catalog:Create(0) end)
      rejects(function() catalog:Create(2,{"subweapon"}) end)
      rejects(function() Inventory.Open("../contract.items.json") end)
      assert(not bag:Remove(0,1).ok and not bag:Add("wood",1.5).ok)
      assert(not bag:Move(1,{},1).ok)
      local removed=bag:Remove(1,1);assert(removed.ok)
      contract_inventory_restore=Inventory.RestoreGroup({{inventory=bag,slots=before},{inventory=equipment,slots=equipment:Snapshot()}})
      assert(contract_inventory_restore.ok and bag:GetSlot(1).count==2)
      local invalid=bag:Snapshot();invalid[1].count=1000
      local current=bag:GetRevision()
      assert(not Inventory.RestoreGroup({{inventory=bag,slots=invalid}}).ok and bag:GetRevision()==current)
      contract_item_definition=catalog:GetItem("hat")
      contract_inventory_slot=equipment:GetSlot(1)
      contract_inventory_snapshot=bag:Snapshot()
      Debug.Log("script_api_contract: representative native calls passed")
    )");
    EmitKey(true); engine.StepSimulation();
    Lua(state,R"(
      local probe=Actor.Find("player"):GetComponentByKey("probe")
      assert(probe.key_held and probe.key_down and not probe.key_up)
      local map=Actor.Find("terrain"):GetComponent("Tilemap")
      assert(map:GetCell("ground",0,0)==2 and map:GetRevision()==1)
      assert(Actor.Find("player"):GetComponent("Rigidbody"):GetPosition().x>1)
    )");
    engine.StepSimulation();
    Lua(state,R"(
      local probe=Actor.Find("player"):GetComponentByKey("probe")
      assert(probe.key_held and not probe.key_down and not probe.key_up)
      local animation=Actor.Find("player"):GetComponent("SpriteAnimation")
      assert(animation.completed and not animation.playing and animation.frame==2)
      assert(probe.completions==1 and probe.events>=4 and probe.last_event.type=="completed")
    )");
    EmitKey(false); engine.StepSimulation();
    Lua(state,"local p=Actor.Find('player'):GetComponentByKey('probe');assert(not p.key_held and not p.key_down and p.key_up)");
    const auto probe=ComponentManager::GetComponentByKey(1,"probe");
    for(const auto &callback:contract["callbacks"].GetArray()) {
        Check(std::string(callback["name"].GetString())=="OnAnimationEvent" && std::string(callback["call"].GetString())=="instance",
              "new host callback requires actual delivery probe");
        Check(Matches(probe["last_event"],callback["parameters"][0]["type"],contract),"delivered animation event differs from schema");
    }
    for(const auto &pair:{std::make_pair("contract_write","SaveWriteResult"),std::make_pair("contract_read","SaveReadResult"),
                         std::make_pair("contract_failure","SaveReadResult"),
                         std::make_pair("contract_inventory_restore","InventoryResult"),
                         std::make_pair("contract_inventory_failure","InventoryResult"),
                         std::make_pair("contract_item_definition","ItemDefinition"),
                         std::make_pair("contract_inventory_slot","InventorySlot")}) {
        rapidjson::Value type(pair.second,static_cast<rapidjson::SizeType>(std::char_traits<char>::length(pair.second)));
        Check(Matches(luabridge::getGlobal(state,pair.first),type,contract),"real return value differs from SDK record/union");
    }
    const auto snapshot=luabridge::getGlobal(state,"contract_inventory_snapshot");
    rapidjson::Value slot_type(rapidjson::StringRef("InventorySlot"));
    Check(snapshot.isTable() && snapshot.length()==4,"inventory snapshot must preserve full capacity");
    for(int i=1;i<=snapshot.length();++i) Check(Matches(snapshot[i],slot_type,contract),"snapshot slot differs from SDK");
}
}
int main() {
    try {
        rapidjson::Document contract;
        contract.Parse(PocketScriptAPI::kContractJson.data(),PocketScriptAPI::kContractJson.size());
        Check(!contract.HasParseError() && contract.IsObject(),"generated contract JSON is malformed");
        Fixture fixture; Engine engine;
        Check(engine.GetLastSceneLoadError().empty(),engine.GetLastSceneLoadError()); engine.InitializeHeadless();
        lua_State *state=ComponentManager::GetComponentByKey(1,"probe").state();
        Coverage coverage; VerifyManifest(state,contract,coverage); Calls(engine,state,contract);
        Check(ComponentManager::GetRuntimeErrorCount()==0,ComponentManager::GetLastRuntimeError());
        Check(lua_gettop(state)==0,"contract probes left an unbalanced Lua stack");
        std::cout << "PASS curated API contract: " << coverage.properties << " property checks, " << coverage.methods
                  << " method/overload names, " << coverage.statics << " static names, " << coverage.constructors << " constructor; real calls/self/nullability/events/save/inventory\n";
        return 0;
    } catch(const std::exception &error) { std::cerr << "FAIL script API contract: " << error.what() << '\n'; return 1; }
}
