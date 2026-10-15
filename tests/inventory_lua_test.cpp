#include "core/Engine.h"
#include "scripting/ComponentManager.h"
#include "shared/resources/ResourcePath.h"
#include "lua.hpp"
#include "LuaBridge/LuaBridge.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
struct Fixture {
    std::filesystem::path previous = ResourcePath::ResourcesRootPath();
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("pocket-inventory-lua-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixture() {
        std::filesystem::create_directories(root / "items");
        std::ofstream(root / "game.config") << "{}";
        std::ofstream(root / "items/test.items.json") << R"({"version":1,"items":[
          {"id":"herb","display_name":"Herb","kind":"food","max_stack":5,"health_restore":1},
          {"id":"ore","display_name":"Ore","kind":"material","max_stack":5},
          {"id":"boots","display_name":"Boots","kind":"armor","max_stack":1,"equipment_slot":"legs","defense":2,"move_speed_bonus":1},
          {"id":"helmet","display_name":"Helmet","kind":"armor","max_stack":1,"equipment_slot":"head","defense":1}
        ]})";
        ResourcePath::SetResourcesRootPath(root);
    }
    ~Fixture() { ResourcePath::SetResourcesRootPath(previous); std::error_code error;std::filesystem::remove_all(root,error); }
};
void Lua(lua_State *state, const char *code) {
    const int top=lua_gettop(state);
    if(luaL_dostring(state,code)!=LUA_OK) {
        const std::string message=lua_tostring(state,-1);lua_settop(state,top);throw std::runtime_error(message);
    }
    if(lua_gettop(state)!=top) throw std::runtime_error("inventory host left Lua stack unbalanced");
}
}
int main() {
    try {
        Fixture fixture; Engine engine; engine.InitializeHeadless();
        auto *state=ComponentManager::GetComponentByKey(0,"missing").state();
        Lua(state,R"lua(
local catalog=Inventory.Open("test.items.json")
assert(type(catalog)=="userdata" and getmetatable(catalog)=="Pocket2D.ItemCatalog")
assert(catalog:GetItem("absent")==nil and catalog:GetItem("HERB")==nil)
local def=catalog:GetItem("boots"); assert(def.max_stack==1 and def.equipment_slot=="legs" and def.move_speed_bonus==1)
def.max_stack=99; assert(catalog:GetItem("boots").max_stack==1)
local bag=catalog:Create(3)
local chest=catalog:Create(2)
local gear=catalog:Create(2,{"head","legs"})
local function unchanged(inv,action)
  local rev=inv:GetRevision(); local r=action(); assert(not r.ok and not r.changed and inv:GetRevision()==rev)
end
assert(bag:GetCapacity()==3 and bag:GetRevision()==0)
assert(bag:Add("herb",4).moved==4 and bag:GetRevision()==1)
assert(bag:Add("ore",5).ok)
assert(bag:Add("herb",8).moved==6 and bag:GetSlot(1).count==5 and bag:GetSlot(3).count==5)
unchanged(bag,function() return bag:Add("herb",2) end)
assert(bag:Remove(3,5).ok)
assert(bag:Split(1,bag,3,2).ok and bag:GetSlot(1).count==3)
assert(bag:Move(3,bag,1).moved==2)
assert(bag:TransferAll(1,chest).moved==5)
assert(bag:Add("boots",1,"boots-1").ok)
local slot=bag:GetSlot(1);slot.count=0;assert(bag:GetSlot(1).count==1)
assert(not bag:Move(1,gear,1).ok and bag:GetSlot(1).instance_id=="boots-1")
assert(bag:Move(1,gear,2).ok and gear:GetSlot(2).instance_id=="boots-1")
assert(gear:Move(2,bag,3).ok and bag:GetSlot(3).instance_id=="boots-1")
assert(not bag:Add("boots",1,"boots-1").ok)
assert(not bag:Add("boots",1).ok and not bag:Add("herb",1,"bad-instance").ok)
local snapshot=bag:Snapshot(); snapshot[2].count=1;assert(bag:GetSlot(2).count==5)
local before=bag:GetRevision(); local c_before=chest:GetRevision()
local bad_chest=chest:Snapshot();bad_chest[2].item_id="missing";bad_chest[2].count=1
assert(not Inventory.RestoreGroup({{inventory=bag,slots=snapshot},{inventory=chest,slots=bad_chest}}).ok)
assert(bag:GetRevision()==before and chest:GetRevision()==c_before and bag:GetSlot(2).count==5)
assert(Inventory.RestoreGroup({{inventory=bag,slots=snapshot},{inventory=chest,slots=chest:Snapshot()}}).ok)
assert(bag:GetSlot(2).count==1 and bag:GetRevision()==before+1 and chest:GetRevision()==c_before)
assert(not Inventory.RestoreGroup({{inventory=bag,slots=snapshot},{inventory=bag,slots=snapshot}}).ok)
-- Every input is raw data. User getters, __len and __pairs must never execute.
local calls=0
local meta={__index=function() calls=calls+1;error("getter executed") end,
 __len=function() calls=calls+1;error("len executed") end,
 __pairs=function() calls=calls+1;error("pairs executed") end}
local raw=bag:Snapshot();for _,entry in ipairs(raw) do setmetatable(entry,meta) end
setmetatable(raw,meta)
assert(Inventory.RestoreGroup(setmetatable({setmetatable({inventory=bag,slots=raw},meta)},meta)).ok)
assert(calls==0)
local function bad(value) assert(not Inventory.RestoreGroup(value).ok);assert(calls==0) end
bad({[2]={inventory=bag,slots=raw}});bad({name="mixed"});bad({{inventory=bag,slots=raw,extra=1}})
bad({{inventory=bag,slots=setmetatable({},meta)}})
local sparse=bag:Snapshot();sparse[2]=nil;bad({{inventory=bag,slots=sparse}})
local wrong=bag:Snapshot();wrong[2].count=1.5;bad({{inventory=bag,slots=wrong}})
wrong=bag:Snapshot();wrong[2].instance_id=nil;bad({{inventory=bag,slots=setmetatable(wrong,meta)}})
local invalid={0,-1,101,math.mininteger,math.maxinteger,1.5,"1",false}
for _,index in ipairs(invalid) do
  unchanged(bag,function() return bag:Remove(index,1) end)
  assert(not pcall(function() bag:GetSlot(index) end))
end
assert(not bag:Move(1,chest,1,0,1).ok and not bag:Add("ore",0/0).ok)
assert(not pcall(function() catalog:Create(101) end))
assert(not pcall(function() catalog:Create(2,{[1]="legs",[3]="head"}) end))
assert(not pcall(function() Inventory.Open("../test.items.json") end))
assert(not pcall(function() Inventory.Open("test.items.json\0") end))
-- Containers retain their immutable catalog after the catalog userdata dies.
local catalog_gc=debug.getmetatable(catalog).__gc
catalog_gc(catalog);catalog_gc(catalog)
assert(not pcall(function() catalog:GetItem("herb") end))
assert(bag:Add("herb",1).ok)
local gc=debug.getmetatable(chest).__gc
gc(chest);gc(chest)
assert(not chest:Add("herb",1).ok and not bag:Move(1,chest,1).ok)
assert(not pcall(function() chest:GetSlot(1) end))
-- Swapping debug metatables cannot reinterpret a different native allocation.
local fake=Vector2(1,2);local original_meta=debug.getmetatable(fake);debug.setmetatable(fake,debug.getmetatable(bag))
assert(not fake:Add("herb",1).ok);gc(fake)
assert(not pcall(function() fake:GetSlot(1) end))
debug.setmetatable(fake,original_meta)
-- Fresh same-file Open is a distinct rule domain. Cross-domain moves fail.
local other=Inventory.Open("test.items.json"):Create(3)
assert(not bag:TransferAll(1,other).ok)
collectgarbage("collect")
)lua");
        std::cout<<"PASS inventory Lua userdata ownership, raw snapshots, atomic restore, invalid calls and identity\n";
        return 0;
    } catch(const std::exception &error) { std::cerr<<"FAIL inventory Lua: "<<error.what()<<'\n';return 1; }
}
