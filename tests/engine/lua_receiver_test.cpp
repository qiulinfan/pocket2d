#include "scripting/LuaBoundary.h"
#include "scripting/ScriptBudget.h"
#include "lua.hpp"
#include "LuaBridge/LuaBridge.h"
#include <iostream>
#include <stdexcept>

namespace {
int native_calls = 0;
struct Box {
    int direct = 7;
    int Member(int n) { ++native_calls; return direct + n; }
    int ConstMember(int n) const { ++native_calls; return direct + n; }
    int Raw(lua_State *state) { ++native_calls; lua_pushinteger(state, direct); return 1; }
    int ConstRaw(lua_State *state) const { ++native_calls; lua_pushinteger(state, direct); return 1; }
    int Read() const { ++native_calls; return direct; }
    int Readonly() const { ++native_calls; return direct + 100; }
    void Write(int value) { ++native_calls; direct = value; }
    int TakeNullable(const Box *other) const { ++native_calls; return other ? other->direct : -1; }
};
struct Derived : Box {};
struct Other { int marker = 91; };
Box host_box;
int static_value = 13;
// These functions intentionally do NOT validate their receiver: the actual
// registered class dispatcher must reject it before native_calls changes.
int Proxy(Box *self, int n) { ++native_calls; return self ? self->direct + n : -777; }
int ProxyRead(const Box *self) { ++native_calls; return self ? self->direct : -777; }
void ProxyWrite(Box *self, int value) { ++native_calls; if (self) self->direct = value; }
int ProxyNullable(Box *self, const Box *other) { ++native_calls; return self ? (other ? other->direct : -1) : -777; }
int Nullable(const Box *value) { ++native_calls; return value ? value->direct : -1; }
const Box *ConstBox() { return &host_box; }
int FreeRaw(lua_State *state) {
    ++native_calls;
    auto *self = LuaBoundary::Cast<Box *>(luabridge::LuaRef::fromStack(state, 1));
    lua_pushinteger(state, self ? self->direct : -777);
    return 1;
}
int RawPropertyGet(lua_State *state) { return FreeRaw(state); }
int RawPropertySet(lua_State *state) {
    ++native_calls;
    auto *self = LuaBoundary::Cast<Box *>(luabridge::LuaRef::fromStack(state, 1));
    const int value = LuaBoundary::Cast<int>(luabridge::LuaRef::fromStack(state, 2));
    if (self) self->direct = value;
    return 0;
}
int Calls() { return native_calls; }
void Check(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
struct Runtime {
    lua_State *state;
    Runtime() {
        ScriptBudget::Configure({1000000, 16 * 1024 * 1024});
        state = ScriptBudget::NewState(); Check(state != nullptr, "cannot create tracked Lua VM");
        luaL_openlibs(state);
    }
    ~Runtime() { ScriptBudget::CloseState(state); }
};
void Register(lua_State *state) {
    luabridge::getGlobalNamespace(state)
        .beginClass<Box>("Box").addConstructor<void (*)()>()
        .addFunction("Member", &Box::Member)
        .addFunction("ConstMember", &Box::ConstMember)
        .addFunction("Raw", &Box::Raw)
        .addFunction("ConstRaw", &Box::ConstRaw)
        .addFunction("FreeRaw", &FreeRaw)
        .addFunction("Proxy", &Proxy)
        .addFunction("Functor", [](Box *self, int n) { ++native_calls; return self ? self->direct + n : -777; })
        .addFunction("TakeNullable", &Box::TakeNullable)
        .addFunction("ProxyNullable", &ProxyNullable)
        .addStaticFunction("StaticNullable", &Nullable)
        .addStaticPropertyReadWrite("static_value", &static_value)
        .addPropertyReadWrite("direct", &Box::direct)
        .addProperty("readonly", &Box::Readonly)
        .addProperty("member", &Box::Read, &Box::Write)
        .addProperty("proxy", &ProxyRead, &ProxyWrite)
        .addProperty("raw_property", &RawPropertyGet, &RawPropertySet)
        .addProperty("functor_property",
            [](const Box *self) { ++native_calls; return self ? self->direct : -777; },
            [](Box *self, int value) { ++native_calls; if (self) self->direct = value; })
        .endClass()
        .deriveClass<Derived, Box>("Derived").addConstructor<void (*)()>().endClass()
        .beginClass<Other>("Other").addConstructor<void (*)()>().addPropertyReadWrite("marker", &Other::marker).endClass()
        .beginNamespace("Host").addFunction("Nullable", &Nullable).addFunction("ConstBox", &ConstBox).addFunction("Calls", &Calls).endNamespace()
        .addFunction("GlobalNullable", &Nullable);
}
void Run(lua_State *state, const char *code) {
    const int top = lua_gettop(state);
    ScriptBudget::RequestScope request;
    const int status = luaL_loadstring(state, code);
    if (status != LUA_OK) { const std::string error = lua_tostring(state, -1); lua_settop(state, top); throw std::runtime_error(error); }
    LuaBoundary::ProtectedCall(state, 0, 0);
    Check(lua_gettop(state) == top && lua_tointeger(state, top) == 917 &&
          std::string(lua_tostring(state, top - 1)) == "receiver-stack-marker", "receiver checks damaged preexisting Lua stack");
    Check(ScriptBudget::GetFailure() == ScriptBudget::Failure::None, "receiver checks unexpectedly exhausted script budget");
}
}
int main() {
    try {
        Runtime runtime; auto *state = runtime.state; Register(state);
        Check(!luabridge::LuaException::areExceptionsEnabled(state), "receiver regression must use production result mode");
        Check(lua_gettop(state) == 0, "registration leaked Lua stack");
        lua_pushliteral(state, "receiver-stack-marker"); lua_pushinteger(state, 917);
        Run(state, R"(
            box=Box();wrong=Other();receiver_rejections=0
            function reject(label,fn,...)
                local before=Host.Calls();local original=box.direct;local marker=wrong.marker
                local ok,err=pcall(fn,...)
                assert(not ok,label..' accepted invalid receiver')
                assert(type(err)=='string' and #err>0,label..' lacked Lua error')
                assert(Host.Calls()==before,label..' entered native body before rejecting self')
                assert(box.direct==original and wrong.marker==marker,label..' mutated native fields')
                receiver_rejections=receiver_rejections+1
            end
            for _,name in ipairs({'Member','ConstMember','Proxy','Functor'}) do
                local fn=box[name]
                reject(name..' nil',fn,nil,3)
                reject(name..' missing',fn)
                reject(name..' wrong userdata',fn,wrong,3)
                reject(name..' wrong table',fn,{},3)
                assert(fn(box,3)==box.direct+3,name..' valid receiver stopped working')
            end
            for _,name in ipairs({'Raw','ConstRaw','FreeRaw'}) do
                local fn=box[name]
                reject(name..' nil',fn,nil)
                reject(name..' missing',fn)
                reject(name..' wrong userdata',fn,wrong)
                assert(fn(box)==box.direct,name..' member Lua C function failed')
            end
            -- Nullable ordinary arguments are a different contract from self.
            assert(Host.Nullable(nil)==-1 and GlobalNullable(nil)==-1 and Box.StaticNullable(nil)==-1)
            assert(Host.Nullable(box)==box.direct and GlobalNullable(box)==box.direct)
            assert(box:TakeNullable(nil)==-1 and box:ProxyNullable(nil)==-1)
            assert(box:TakeNullable(box)==box.direct and box:ProxyNullable(box)==box.direct)
            reject('nullable member bad self',box.TakeNullable,nil,nil)
            reject('nullable proxy bad self',box.ProxyNullable,nil,nil)
        )");
        Run(state, R"(
            -- Discover actual registered getter/setter closures through Lua's
            -- debug reflection, without calling vendor implementation symbols.
            local mt=debug.getmetatable(box)
            local getters,setters
            for _,candidate in pairs(mt) do
                if type(candidate)=='table' and type(rawget(candidate,'proxy'))=='function' then
                    local ok,value=pcall(candidate.proxy,box,box.direct)
                    if ok and type(value)=='number' then getters=candidate end
                    if ok and value==nil then setters=candidate end
                end
            end
            assert(getters and setters,'could not locate real registered property closures')
            for _,name in ipairs({'direct','readonly','member','proxy','functor_property','raw_property'}) do
                local fn=rawget(getters,name);assert(type(fn)=='function')
                reject(name..' getter nil',fn,nil)
                reject(name..' getter missing',fn)
                reject(name..' getter wrong',fn,wrong)
                assert(type(fn(box))=='number',name..' getter valid receiver failed')
            end
            for _,name in ipairs({'direct','member','proxy','functor_property','raw_property'}) do
                local fn=rawget(setters,name);assert(type(fn)=='function')
                reject(name..' setter nil',fn,nil,17)
                reject(name..' setter missing',fn)
                reject(name..' setter wrong',fn,wrong,17)
                fn(box,17);assert(box.direct==17,name..' setter valid receiver failed')
            end
            reject('readonly assignment',function()box.readonly=1 end)
            box.direct=8;box.member=9;box.proxy=10;box.functor_property=11;box.raw_property=11
            assert(box.direct==11 and box.readonly==111)
            -- Invalid direct invocation of public metatables is a receiver
            -- contract check, not a claim to accept forged userdata/metatables.
            reject('index nil',mt.__index,nil,'direct')
            reject('index missing',mt.__index)
            reject('index number',mt.__index,12,'direct')
            reject('newindex nil',mt.__newindex,nil,'direct',19)
            reject('newindex missing',mt.__newindex)
            reject('newindex number',mt.__newindex,12,'direct',19)
            local unmanaged=setmetatable({}, {})
            reject('index unmanaged table',mt.__index,unmanaged,'direct')
            reject('newindex unmanaged table',mt.__newindex,unmanaged,'direct',19)
            assert(next(unmanaged)==nil,'malformed receiver gained a field')
            assert(mt.__index(box,'direct')==11)
            mt.__newindex(box,'direct',12);assert(box.direct==12)
            -- Generic raw metadata functions are not an exact-class API. An
            -- unrelated userdata's unknown field may resolve to nil, and real
            -- class/static tables remain supported.
            local ok,missing=pcall(mt.__index,wrong,'unknown_field')
            assert(ok and missing==nil)
            local static_mt=debug.getmetatable(Box)
            assert(static_mt.__index(Box,'static_value')==13)
            static_mt.__newindex(Box,'static_value',23);assert(Box.static_value==23)
            reject('static index nil',static_mt.__index,nil,'static_value')
            reject('static newindex nil',static_mt.__newindex,nil,'static_value',99)
            assert(Box.static_value==23)
        )");
        Run(state, R"(
            derived=Derived();derived.direct=20
            assert(derived:Member(2)==22 and derived:ConstMember(2)==22 and derived:Proxy(2)==22 and derived:Functor(2)==22)
            derived.member=21;derived.proxy=22;derived.functor_property=23;derived.raw_property=23
            assert(derived.direct==23 and derived.readonly==123 and derived:Raw()==23 and derived:ConstRaw()==23 and derived:FreeRaw()==23)
            assert(derived:TakeNullable(nil)==-1 and derived:ProxyNullable(nil)==-1)
            local constant=Host.ConstBox()
            assert(constant:ConstMember(1)==8 and constant:ConstRaw()==7 and constant:TakeNullable(nil)==-1)
            reject('nonconst method on const self',constant.Member,constant,1)
            reject('const field write',function()constant.direct=99 end)
            assert(constant.direct==7)
            assert(receiver_rejections>=50)
        )");
        lua_pop(state, 2);
        Check(lua_gettop(state) == 0, "final receiver stack not restored");
        std::cout << "lua_receiver_test: actual class dispatch rejects invalid self and preserves nullable arguments/inheritance\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "lua_receiver_test: " << error.what() << '\n'; return 1;
    }
}
