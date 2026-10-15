#include "scripting/LuaBoundary.h"
#include "scripting/ScriptBudget.h"
#include "scripting/ComponentManager.h"
#include "core/Engine.h"
#include "shared/resources/ResourcePath.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void Check(bool condition,const char *message){if(!condition)throw std::runtime_error(message);}
void Load(lua_State *state,const char *code){
    Check(luaL_loadbuffer(state,code,std::char_traits<char>::length(code),"@boundary_fixture.lua")==LUA_OK,"fixture compile failed");
    LuaBoundary::ProtectedCall(state,0,0);
}
struct VM {
    lua_State *state;
    VM(){ScriptBudget::Configure({});state=ScriptBudget::NewState();Check(state,"VM creation failed");luaL_openlibs(state);}
    ~VM(){ScriptBudget::CloseState(state);}
};
void DirectBoundary(){
    VM vm;auto *state=vm.state;
    const auto original=lua_atpanic(state,nullptr);lua_atpanic(state,original);
    Check(!luabridge::LuaException::areExceptionsEnabled(state),"library throw mode must remain off");
    const auto retained=lua_atpanic(state,original);Check(retained==original,"binding initialization replaced owner panic");
    Load(state,R"(
        disabled=false; enabled=true; wrong={}
        function zero() end
        function one() return 42 end
        function many() return 42,'ignored',{} end
        function failed() error('BOUNDARY_SENTINEL') end
        function bytes() error(string.char(255,0)..'TAIL_SENTINEL') end
        function huge() error(string.rep('界',10000)) end
    )");
    lua_pushliteral(state,"outer-marker");lua_pushinteger(state,123);
    const auto balanced=[&]{Check(lua_gettop(state)==2&&lua_tointeger(state,2)==123&&std::string(lua_tostring(state,1))=="outer-marker","boundary damaged outer stack");};
    for(const char *name:{"zero","one","many"}){LuaBoundary::CallDiscard(luabridge::getGlobal(state,name));balanced();}
    Check(!LuaBoundary::Cast<bool>(luabridge::getGlobal(state,"disabled"))&&LuaBoundary::Cast<bool>(luabridge::getGlobal(state,"enabled")),"checked cast confused value with success");
    Check(!LuaBoundary::IsEnabled(luabridge::getGlobal(state,"disabled"))&&
          LuaBoundary::IsEnabled(luabridge::getGlobal(state,"enabled"))&&
          LuaBoundary::IsEnabled(luabridge::getGlobal(state,"wrong"))&&
          LuaBoundary::IsEnabled(luabridge::LuaRef(state)),"single-read enabled policy changed false/true/table/nil");balanced();
    bool rejected=false;try{(void)LuaBoundary::Cast<int>(luabridge::getGlobal(state,"wrong"));}catch(const LuaBoundary::Error &){rejected=true;}
    Check(rejected,"invalid conversion was silently accepted");balanced();
    for(const char *name:{"failed","bytes","huge"}){
        std::string message;
        try{LuaBoundary::CallDiscard(luabridge::getGlobal(state,name));}catch(const LuaBoundary::Error &error){message=error.what();}
        Check(!message.empty()&&message.size()<=4096&&message.find('\0')==std::string::npos,"diagnostic unbounded/empty/NUL");
        Check(message.find("boundary_fixture.lua")!=std::string::npos&&message.find("stack traceback")!=std::string::npos,"diagnostic lost source or traceback");
        if(std::string(name)=="failed")Check(message.find("BOUNDARY_SENTINEL")!=std::string::npos,"original error lost");
        if(std::string(name)=="bytes")Check(message.find("\\xFF\\x00TAIL_SENTINEL")!=std::string::npos,"raw error suffix lost at NUL/invalidUTF8");
        balanced();lua_getglobal(state,"utf8");lua_getfield(state,-1,"len");lua_pushlstring(state,message.data(),message.size());
        LuaBoundary::ProtectedCall(state,1,1);Check(lua_isinteger(state,-1),"diagnostic invalid UTF8");lua_pop(state,2);balanced();
        LuaBoundary::CallDiscard(luabridge::getGlobal(state,"one"));balanced();
    }
    lua_getglobal(state,"many");LuaBoundary::ProtectedCall(state,0,1);Check(lua_tointeger(state,-1)==42,"protected result retention failed");lua_pop(state,1);balanced();
    lua_pop(state,2);
}
struct Fixture {
    std::filesystem::path previous=ResourcePath::ResourcesRootPath();
    std::filesystem::path root=std::filesystem::temp_directory_path()/("pocket-lua-boundary-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixture(){
        std::filesystem::create_directories(root/"component_types");std::filesystem::create_directories(root/"scenes");
        std::ofstream(root/"game.config")<<R"({"initial_scene":"main"})";
        std::ofstream(root/"scenes/main.scene")<<R"({"actors":[{"uid":1,"name":"active","components":{"logic":{"type":"Probe"}}},{"uid":2,"name":"disabled","components":{"logic":{"type":"Probe","enabled":false}}}]})";
        std::ofstream(root/"component_types/Probe.lua")<<R"(
            Probe={starts=0,updates=0,late=0,events=0,
                OnStart=function(self)
                    self.starts=self.starts+1
                    Event.Subscribe('tick',self,self.Receive)
                    return false,'ignored',{}
                end,
                Receive=function(self,event)self.events=self.events+1;return 1,2,3 end,
                OnUpdate=function(self)self.updates=self.updates+1;Event.Publish('tick',{});return {} end,
                OnLateUpdate=function(self)self.late=self.late+1;return false end}
        )";
        ResourcePath::SetResourcesRootPath(root);
    }
    ~Fixture(){ResourcePath::SetResourcesRootPath(previous);std::error_code ignored;std::filesystem::remove_all(root,ignored);}
};
void RealRuntime(){
    Fixture fixture;Engine engine;engine.InitializeHeadless();
    engine.StepSimulation();engine.StepSimulation();
    auto active=ComponentManager::GetComponentByKey(1,"logic");auto disabled=ComponentManager::GetComponentByKey(2,"logic");
    Check(!luabridge::LuaException::areExceptionsEnabled(active.state()),"real runtime enabled library throw mode");
    Check(LuaBoundary::Cast<int>(active["starts"])==1&&LuaBoundary::Cast<int>(active["updates"])==2&&LuaBoundary::Cast<int>(active["late"])==2,"returned callback values changed lifecycle");
    Check(LuaBoundary::Cast<int>(disabled["updates"])==0&&LuaBoundary::Cast<int>(disabled["late"])==0,"enabled=false ran lifecycle");
    Check(LuaBoundary::Cast<int>(active["events"])>0,"real event callback did not run");
    Check(ComponentManager::GetRuntimeErrorCount()==0,"ignored callback returns were reported as errors");
    Load(active.state(),"Actor.Find('active'):GetComponentByKey('logic').OnUpdate=function() error('REAL_CALLBACK_SENTINEL') end");
    engine.StepSimulation();Check(ComponentManager::GetRuntimeErrorCount()==1,"real callback error bypassed runtime counter");
    const auto &error=ComponentManager::GetLastRuntimeError();
    Check(error.find("REAL_CALLBACK_SENTINEL")!=std::string::npos&&error.find("boundary_fixture.lua")!=std::string::npos&&error.find("stack traceback")!=std::string::npos,"real runtime sink lost diagnostic context");
}
}
int main(){try{DirectBoundary();Check(ScriptBudget::HeapBytesUsed()==0,"direct VM leaked tracked bytes");RealRuntime();std::cout<<"lua_boundary_test: checked values, discard returns, diagnostics, panic and real lifecycle passed\n";return 0;}catch(const std::exception &error){std::cerr<<error.what()<<'\n';return 1;}}
