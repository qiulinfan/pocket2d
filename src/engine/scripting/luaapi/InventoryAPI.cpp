#include "RegistrationDetail.h"
#include "shared/inventory/InventoryModel.h"
#include "lua.hpp"
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using Catalog = std::shared_ptr<const ItemCatalog::Data>;
using PocketInventory::Model;
using PocketInventory::Result;
using PocketInventory::Slot;
using InventoryHandle = std::shared_ptr<Model>;
constexpr const char *catalog_meta = "Pocket2D.ItemCatalog";
constexpr const char *inventory_meta = "Pocket2D.Inventory";

// Explicit lifetime marker also makes a manually repeated __gc harmless. No
// userdata exports a raw slot pointer or an actor reference.
template<class T> struct Box {
    const void *tag;
    bool alive;
    alignas(T) unsigned char storage[sizeof(T)];
    T *Value() { return std::launder(reinterpret_cast<T *>(storage)); }
};
template<class T> const void *Tag() { static const char tag = 0; return &tag; }
template<class T> Box<T> *CheckedBox(lua_State *state, int index, const char *meta) {
    auto *box = static_cast<Box<T> *>(luaL_testudata(state, index, meta));
    if (!box || lua_rawlen(state, index) != sizeof(Box<T>) || box->tag != Tag<T>() || !box->alive)
        throw std::invalid_argument("expected a live inventory/catalog userdata");
    return box;
}
template<class T, class... Args> void PushBox(lua_State *state, const char *meta, Args&&... args) {
    auto *box = static_cast<Box<T> *>(lua_newuserdatauv(state, sizeof(Box<T>), 0));
    box->tag = Tag<T>(); box->alive = false;
    luaL_getmetatable(state, meta); lua_setmetatable(state, -2);
    new (box->storage) T(std::forward<Args>(args)...);
    box->alive = true;
}
template<class T> int Destroy(lua_State *state, const char *meta) {
    auto *box = static_cast<Box<T> *>(luaL_testudata(state, 1, meta));
    if (box && lua_rawlen(state, 1) == sizeof(Box<T>) && box->tag == Tag<T>() && box->alive) {
        box->Value()->~T(); box->alive = false;
    }
    return 0;
}
int CatalogGC(lua_State *state) { return Destroy<Catalog>(state, catalog_meta); }
int InventoryGC(lua_State *state) { return Destroy<InventoryHandle>(state, inventory_meta); }
InventoryHandle InventoryAt(lua_State *state, int index) {
    return *CheckedBox<InventoryHandle>(state, index, inventory_meta)->Value();
}
Catalog CatalogAt(lua_State *state, int index) {
    return *CheckedBox<Catalog>(state, index, catalog_meta)->Value();
}
void Arity(lua_State *state, int minimum, int maximum) {
    if (lua_gettop(state) < minimum || lua_gettop(state) > maximum)
        throw std::invalid_argument("incorrect inventory API argument count");
}
int Integer(lua_State *state, int index) {
    if (!lua_isinteger(state, index)) throw std::invalid_argument("expected an integer");
    const auto value = lua_tointeger(state, index);
    if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max())
        throw std::invalid_argument("integer outside supported range");
    return static_cast<int>(value);
}
int SlotIndex(lua_State *state, int index) {
    const int value = Integer(state, index);
    if (value < 1 || value > 100) throw std::invalid_argument("slot index must be in 1..100");
    return value - 1;
}
std::string String(lua_State *state, int index, std::size_t maximum = 4096) {
    if (lua_type(state, index) != LUA_TSTRING) throw std::invalid_argument("expected a string");
    std::size_t length = 0;
    const char *value = lua_tolstring(state, index, &length);
    if (length > maximum) throw std::invalid_argument("inventory string exceeds limit");
    return {value, length};
}
void TextField(lua_State *state, const char *key, const std::string &text) {
    lua_pushlstring(state, text.data(), text.size()); lua_setfield(state, -2, key);
}
void IntField(lua_State *state, const char *key, lua_Integer value) {
    lua_pushinteger(state, value); lua_setfield(state, -2, key);
}
int PushResult(lua_State *state, const Result &result) {
    lua_createtable(state, 0, 5);
    lua_pushboolean(state, result.ok); lua_setfield(state, -2, "ok");
    TextField(state, "code", result.code);
    IntField(state, "moved", result.moved); IntField(state, "remaining", result.remaining);
    lua_pushboolean(state, result.changed); lua_setfield(state, -2, "changed");
    return 1;
}
void PushSlot(lua_State *state, const Slot &slot) {
    lua_createtable(state, 0, 3);
    TextField(state, "item_id", slot.item_id); IntField(state, "count", slot.count);
    TextField(state, "instance_id", slot.instance_id);
}
// Raw parsing intentionally does not call __index, __len, __pairs or getters.
// Bounds include all keys; a sparse array or unexpected string key is rejected.
int DenseLength(lua_State *state, int index, int maximum) {
    if (lua_type(state, index) != LUA_TTABLE) throw std::invalid_argument("expected a dense array");
    index = lua_absindex(state, index);
    int count = 0, largest = 0;
    lua_pushnil(state);
    while (lua_next(state, index)) {
        if (!lua_isinteger(state, -2)) throw std::invalid_argument("array keys must be integers");
        const auto key = lua_tointeger(state, -2);
        if (key < 1 || key > maximum || ++count > maximum)
            throw std::invalid_argument("array exceeds limit or is sparse");
        if (key > largest) largest = static_cast<int>(key);
        lua_pop(state, 1);
    }
    if (largest != count) throw std::invalid_argument("array must be dense");
    return count;
}
void Fields(lua_State *state, int index, std::initializer_list<std::string_view> fields) {
    if (lua_type(state, index) != LUA_TTABLE) throw std::invalid_argument("expected an inventory record");
    index = lua_absindex(state, index);
    lua_pushnil(state);
    while (lua_next(state, index)) {
        const auto key = String(state, -2, 64);
        bool known = false;
        for (auto field : fields) if (field == key) known = true;
        if (!known) throw std::invalid_argument("unknown inventory record field");
        lua_pop(state, 1);
    }
}
void RawField(lua_State *state, int index, const char *key) {
    index = lua_absindex(state, index); lua_pushstring(state, key); lua_rawget(state, index);
}
std::vector<Slot> ParseSlots(lua_State *state, int index) {
    index = lua_absindex(state, index);
    const int length = DenseLength(state, index, 100);
    std::vector<Slot> result; result.reserve(length);
    for (int i = 1; i <= length; ++i) {
        lua_rawgeti(state, index, i);
        Fields(state, -1, {"item_id", "count", "instance_id"});
        Slot slot;
        RawField(state, -1, "item_id"); slot.item_id = String(state, -1, 64); lua_pop(state, 1);
        RawField(state, -1, "count"); slot.count = Integer(state, -1); lua_pop(state, 1);
        RawField(state, -1, "instance_id"); slot.instance_id = String(state, -1, 64); lua_pop(state, 1);
        result.push_back(std::move(slot)); lua_pop(state, 1);
    }
    return result;
}
using Function = int (*)(lua_State *);
template<Function function, bool mutation = false> int Guard(lua_State *state) {
    const int top = lua_gettop(state);
    // Finish unwinding C++ temporaries before lua_error's longjmp.
    try { return function(state); }
    catch (const std::exception &error) {
        lua_settop(state, top);
        if constexpr (mutation) return PushResult(state, {false, "invalid_argument", 0, 0, false});
        else lua_pushstring(state, error.what());
    }
    return lua_error(state);
}
int Open(lua_State *state) {
    Arity(state, 1, 1);
    const auto resource = String(state, 1);
    auto data = std::make_shared<const ItemCatalog::Data>(ItemCatalog::LoadResource(resource));
    PushBox<Catalog>(state, catalog_meta, std::move(data)); return 1;
}
int Create(lua_State *state) {
    Arity(state, 2, 3);
    auto catalog = CatalogAt(state, 1);
    const int capacity = Integer(state, 2);
    std::vector<std::string> filters;
    if (!lua_isnoneornil(state, 3)) {
        const int length = DenseLength(state, 3, 100); filters.reserve(length);
        for (int i = 1; i <= length; ++i) {
            lua_rawgeti(state, 3, i); filters.push_back(String(state, -1, 64)); lua_pop(state, 1);
        }
    }
    PushBox<InventoryHandle>(state, inventory_meta, std::make_shared<Model>(catalog, capacity, std::move(filters))); return 1;
}
int GetItem(lua_State *state) {
    Arity(state, 2, 2);
    const auto catalog = CatalogAt(state, 1);
    const auto *item = ItemCatalog::Find(*catalog, String(state, 2, 64));
    if (!item) { lua_pushnil(state); return 1; }
    lua_createtable(state, 0, 14);
    TextField(state, "id", item->id); TextField(state, "display_name", item->display_name);
    TextField(state, "description", item->description); TextField(state, "kind", item->kind);
    TextField(state, "equipment_slot", item->equipment_slot);
    IntField(state, "max_stack", item->max_stack); IntField(state, "health_restore", item->health_restore);
    IntField(state, "attack", item->attack); IntField(state, "defense", item->defense);
    lua_pushnumber(state, item->move_speed_bonus); lua_setfield(state, -2, "move_speed_bonus");
    TextField(state, "icon", item->icon); IntField(state, "tool_power", item->tool_power);
    TextField(state, "places", item->places); TextField(state, "use", item->use); return 1;
}
int GetSlot(lua_State *state) {
    Arity(state, 2, 2); const auto inventory = InventoryAt(state, 1); const int index = SlotIndex(state, 2);
    if (index >= inventory->Capacity()) throw std::invalid_argument("slot index exceeds container capacity");
    const auto slot = inventory->Slots()[index];
    PushSlot(state, slot); return 1;
}
int GetCapacity(lua_State *state) {
    Arity(state, 1, 1); lua_pushinteger(state, InventoryAt(state, 1)->Capacity()); return 1;
}
int GetRevision(lua_State *state) {
    Arity(state, 1, 1); const auto revision = InventoryAt(state, 1)->Revision();
    if (revision > static_cast<std::uint64_t>(std::numeric_limits<lua_Integer>::max()))
        throw std::overflow_error("inventory revision exceeds Lua integer range");
    lua_pushinteger(state, static_cast<lua_Integer>(revision)); return 1;
}
int Snapshot(lua_State *state) {
    Arity(state, 1, 1); const auto inventory = InventoryAt(state, 1);
    const auto slots = inventory->Slots();
    lua_createtable(state, inventory->Capacity(), 0);
    for (int i = 0; i < inventory->Capacity(); ++i) { PushSlot(state, slots[i]); lua_rawseti(state, -2, i + 1); }
    return 1;
}
int Add(lua_State *state) {
    Arity(state, 3, 4); auto inventory = InventoryAt(state, 1);
    Slot request{String(state, 2, 64), Integer(state, 3), ""};
    if (!lua_isnoneornil(state, 4)) request.instance_id = String(state, 4, 64);
    return PushResult(state, inventory->AddUpTo(request));
}
int Remove(lua_State *state) {
    Arity(state, 3, 3); return PushResult(state, InventoryAt(state, 1)->RemoveExact(SlotIndex(state, 2), Integer(state, 3)));
}
int Move(lua_State *state) {
    Arity(state, 4, 6);
    const int count = lua_isnoneornil(state, 5) ? 0 : Integer(state, 5);
    bool swap = true;
    if (!lua_isnoneornil(state, 6)) {
        if (!lua_isboolean(state, 6)) throw std::invalid_argument("allowSwap must be boolean");
        swap = lua_toboolean(state, 6);
    }
    return PushResult(state, InventoryAt(state, 1)->Move(SlotIndex(state, 2), *InventoryAt(state, 3), SlotIndex(state, 4), count, swap));
}
int Split(lua_State *state) {
    Arity(state, 5, 5);
    return PushResult(state, InventoryAt(state, 1)->Split(SlotIndex(state, 2), *InventoryAt(state, 3), SlotIndex(state, 4), Integer(state, 5)));
}
int TransferAll(lua_State *state) {
    Arity(state, 3, 3); return PushResult(state, InventoryAt(state, 1)->TransferAll(SlotIndex(state, 2), *InventoryAt(state, 3)));
}
int RestoreGroup(lua_State *state) {
    Arity(state, 1, 1); const int length = DenseLength(state, 1, 100);
    std::vector<Model::RestoreEntry> entries; entries.reserve(length);
    std::vector<InventoryHandle> owners; owners.reserve(length);
    for (int i = 1; i <= length; ++i) {
        lua_rawgeti(state, 1, i); Fields(state, -1, {"inventory", "slots"});
        RawField(state, -1, "inventory"); auto inventory = InventoryAt(state, -1); owners.push_back(inventory); lua_pop(state, 1);
        RawField(state, -1, "slots"); auto slots = ParseSlots(state, -1); lua_pop(state, 1);
        entries.emplace_back(inventory.get(), std::move(slots)); lua_pop(state, 1);
    }
    return PushResult(state, Model::RestoreGroup(entries));
}
void Metatable(lua_State *state, const char *name, const luaL_Reg *methods, lua_CFunction gc) {
    luaL_newmetatable(state, name);
    lua_pushcfunction(state, gc); lua_setfield(state, -2, "__gc");
    lua_pushstring(state, name); lua_setfield(state, -2, "__metatable");
    lua_newtable(state); luaL_setfuncs(state, methods, 0); lua_setfield(state, -2, "__index");
    lua_pop(state, 1);
}
} // namespace
namespace APIRegistrationDetail {
void RegisterInventoryAPI() {
    const luaL_Reg catalog_methods[] = {
        {"GetItem", Guard<GetItem>}, {"Create", Guard<Create>}, {nullptr, nullptr}};
    const luaL_Reg inventory_methods[] = {
        {"GetSlot", Guard<GetSlot>}, {"GetCapacity", Guard<GetCapacity>}, {"GetRevision", Guard<GetRevision>},
        {"Snapshot", Guard<Snapshot>}, {"Add", Guard<Add, true>}, {"Remove", Guard<Remove, true>},
        {"Move", Guard<Move, true>}, {"Split", Guard<Split, true>}, {"TransferAll", Guard<TransferAll, true>}, {nullptr, nullptr}};
    Metatable(LuaState(), catalog_meta, catalog_methods, CatalogGC);
    Metatable(LuaState(), inventory_meta, inventory_methods, InventoryGC);
    lua_newtable(LuaState());
    lua_pushcfunction(LuaState(), Guard<Open>); lua_setfield(LuaState(), -2, "Open");
    lua_pushcfunction(LuaState(), (Guard<RestoreGroup, true>)); lua_setfield(LuaState(), -2, "RestoreGroup");
    lua_setglobal(LuaState(), "Inventory");
}
}
