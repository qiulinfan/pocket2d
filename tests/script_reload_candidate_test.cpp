#include "scripting/ScriptReloadCandidate.h"
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void Check(bool condition, const std::string &message) { if (!condition) throw std::runtime_error(message); }
void Lua(lua_State *state, const char *source) {
    const int top = lua_gettop(state);
    const int result = luaL_dostring(state, source);
    const auto error = result == LUA_OK ? std::string() : std::string(lua_tostring(state, -1));
    lua_settop(state, top); Check(result == LUA_OK, error);
}
struct VM {
    lua_State *state = luaL_newstate();
    VM() { Check(state != nullptr, "live Lua allocation failed"); luaL_openlibs(state); }
    ~VM() { lua_close(state); }
};
using ScriptReloadCandidate::Source;
auto CheckedStage(lua_State *state, const std::vector<Source> &sources) {
    const int top = lua_gettop(state);
    auto batch = ScriptReloadCandidate::Stage(state, sources);
    Check(lua_gettop(state) == top, "Stage changed the live stack height");
    return batch;
}
void Commit(lua_State *state, const ScriptReloadCandidate::Batch &batch) {
    for (const auto &[name, value] : batch.exports) { value.push(state); lua_setglobal(state, name.c_str()); }
}
void Reject(lua_State *state, const std::string &code, const std::string &fragment = "", const std::string &name = "Candidate") {
    const auto batch = CheckedStage(state, {{name, "candidate.lua", code}});
    Check(!batch.ok && batch.exports.empty() && batch.diagnostics.size() == 1, "invalid candidate accepted: " + code.substr(0, 90));
    const auto &diagnostic = batch.diagnostics.front();
    Check(diagnostic.type == name && diagnostic.resource == "candidate.lua" && !diagnostic.stage.empty() && !diagnostic.message.empty(), "missing structured diagnostic");
    Check(fragment.empty() || diagnostic.message.find(fragment) != std::string::npos,
          "unexpected diagnostic: " + diagnostic.message + "; wanted " + fragment);
    Lua(state, "assert(Candidate==old_candidate and Candidate.value==77 and host_changes==0 and Unexpected==nil)");
}
void Run() {
    Check(ScriptReloadCandidate::SanitizeDiagnosticText(std::string("bad\xff\0", 5)) == "bad\\xFF\\x00",
          "public diagnostic sanitizer differs from candidate errors");
    VM vm; auto *state = vm.state;
    Lua(state, R"(
        Candidate={value=77};old_candidate=Candidate;host_changes=0
        Actor={Destroy=function(value) assert(value=="live-call");host_changes=host_changes+1 end}
        Save={Write=function() host_changes=host_changes+1 end}
        LiveProbe=function(value) return value+100 end
    )");
    lua_pushliteral(state, "live stack sentinel");
    auto batch = CheckedStage(state, {{"First", "components/First.lua", R"(
        local count=0
        local shared={value=5};shared.self=shared
        local function recurse(n) if n==0 then return count end return recurse(n-1)+1 end
        First={x=7,a=shared,b=shared,
            inc=function() count=count+1;return count end,
            get=function() return count end,recursive=recurse,
            absolute=math.abs,library=math,
            inherited=setmetatable({}, {__index={value=9}}),
            host=function(self) Actor.Destroy("live-call");return LiveProbe(self.x) end}
    )"}, {"Second", "components/Second.lua", R"(
        local first=First
        Second={first=first,inc=first.inc,read=function() return first.get() end}
    )"}});
    Check(batch.ok && batch.exports.size() == 2, batch.diagnostics.empty() ? "valid capture graph failed" : batch.diagnostics[0].message);
    Lua(state, "assert(First==nil and Second==nil and host_changes==0)");
    Commit(state, batch);
    Lua(state, R"(
        assert(First.a==First.b and First.a.self==First.a and First.a.value==5)
        assert(Second.first==First and Second.inc==First.inc)
        assert(First.inc()==1 and Second.read()==1 and Second.inc()==2 and First.get()==2)
        assert(First.recursive(4)==6 and First.inherited.value==9)
        assert(First.absolute(-4)==4 and First.absolute==math.abs and First.library==math)
        assert(First:host()==107 and host_changes==1)
        host_changes=0
    )");
    Check(std::string(lua_tostring(state, 1)) == "live stack sentinel", "live stack content changed");

    Reject(state, "Candidate={\n bad=\n}");
    auto syntax = CheckedStage(state, {{"Syntax", "syntax.lua", "Syntax={\n x =\n}"}});
    Check(!syntax.ok && syntax.diagnostics[0].stage == "compile" && syntax.diagnostics[0].line > 0, "syntax diagnostic lost source line");
    Reject(state, "error('top-level failure')", "top-level failure");
    for (const char *code : {"error(string.rep('a',20*1024*1024),0)",
                             "error(string.rep('a',10*1024*1024))",
                             "error(string.rep('你',2000))",
                             "error(string.char(255,192,128,237,160,128,240,128,128,128,0))"}) {
        const auto failed = CheckedStage(state, {{"Candidate", "candidate.lua", code}});
        Check(!failed.ok && failed.exports.empty() && failed.diagnostics.size() == 1, "large/binary error was accepted");
        const auto &message = failed.diagnostics[0].message;
        Check(!message.empty() && message.size() <= 4096 && message.find('\0') == std::string::npos, "diagnostic exceeded text boundary");
        lua_pushlstring(state, message.data(), message.size()); lua_setglobal(state, "diagnostic_text");
        Lua(state, "assert(utf8.len(diagnostic_text));assert(Candidate==old_candidate and Candidate.value==77 and host_changes==0)");
        const bool heap_limited = message.find("heap limit") != std::string::npos;
        if (std::string(code).find("rep") != std::string::npos) Check(heap_limited || message.find("[truncated]") != std::string::npos, "large error missing truncation marker");
        else Check(message.find("\\xFF") != std::string::npos && message.find("\\x00") != std::string::npos, "invalid bytes/control byte were not escaped");
        if (!heap_limited && std::string(code).find(",0)") == std::string::npos) Check(failed.diagnostics[0].line == 1, "sanitization lost source line prefix");
    }
    Reject(state, "local Candidate={}", "newly export");
    Reject(state, "Candidate=42", "newly export");
    Reject(state, "Candidate={};Unexpected=1", "unexpected candidate global");
    Reject(state, "Candidate={};_G.Unexpected=1", "unexpected candidate global");
    Reject(state, "rawset(_G,'Unexpected',1);Candidate={}", "unexpected candidate global");
    Reject(state, "local Missing=Missing;Candidate={f=function() return Missing.value end}", "unknown candidate global");
    Reject(state, "local Missing=rawget(_G,'Missing');Candidate={f=function() return Missing.value end}", "unknown candidate global");
    Reject(state, "local destroy=Actor;Candidate={f=function() return destroy end}", "unavailable");
    Reject(state, "local random=math.random;Candidate={f=function() return random() end}", "unavailable");
    Reject(state, "local match=string.match;Candidate={f=function() return match('a','a') end}", "unavailable");
    Reject(state, "local match=('abc').match;Candidate={f=function() return match('a','a') end}", "unavailable");
    Reject(state, "local match=rawget(string,'match');Candidate={f=function() return match('a','a') end}", "unavailable");
    auto selected_nil = CheckedStage(state, {{"Selected", "selected.lua", "Selected=Selected or {};assert(math.not_an_api==nil and ({}).missing==nil);Selected.value=1"}});
    Check(selected_nil.ok, "selected-export or ordinary-field nil semantics were lost");
    auto bundle_loader = CheckedStage(state, {{"Bundled", "bundle.lua", R"(
        local originalRequire=require
        local modules={value=function() return 9 end}
        local function require(name)
            if modules[name] then return modules[name]() end
            return originalRequire(name)
        end
        Bundled={value=require("value")}
    )"}});
    Check(bundle_loader.ok, "known-nil require broke the TSTL local bundle loader");
    Reject(state, "Candidate={actor={}}", "actor/key");
    Reject(state, "Candidate={key='owned'}", "actor/key");
    Reject(state, "Candidate={[1]='array'}", "keys must be strings");
    Reject(state, "Candidate=setmetatable({}, {})", "plain tables");
    for (const char *name : {"math", "Actor", "pcall", "require", "io", "_G"})
        Reject(state, std::string(name) + "={}", "", name);
    Reject(state, "Actor.Destroy('live-call');Candidate={}");
    Reject(state, "Save.Write('file',1,{});Candidate={}");
    Reject(state, "io.open('should-not-exist','w');Candidate={}");
    Reject(state, "os.execute('true');Candidate={}");
    Reject(state, "require('anything');Candidate={}");
    Reject(state, "load('return 1')();Candidate={}");
    Reject(state, "dofile('anything');Candidate={}");
    Reject(state, "coroutine.create(function() end);Candidate={}");
    Reject(state, "math.random();Candidate={}");
    Reject(state, "math.randomseed(99);Candidate={}");
    Lua(state, "math.randomseed(123,456);expected_random=math.random();math.randomseed(123,456)");
    Reject(state, "math.random();Candidate={}");
    Lua(state, "assert(math.random()==expected_random)");

    for (const char *code : {
            "math.abs=nil;Candidate={}", "rawset(math,'abs',nil);Candidate={}",
            "math={};Candidate={}", "_VERSION='fake';Candidate={}",
            "setmetatable(math,{});Candidate={}", "rawset(string,'new',true);Candidate={}",
            "local m=getmetatable('');m.__index={};Candidate={}"}) Reject(state, code);
    // A temporary mutation restored before export must still be rejected.
    Reject(state, "local abs=math.abs;math.abs=nil;math.abs=abs;Candidate={}", "readonly");
    auto introspection = CheckedStage(state, {{"Introspection", "inspect.lua", R"(
        assert(rawget(math,"abs")==math.abs)
        local found=false;for k,v in pairs(math) do if k=="abs" then found=true end end
        assert(found)
        local _,state= pairs(math)
        assert(state==math)
        Introspection={lookup=rawget,assign=rawset,next=next,setmeta=setmetatable}
    )"}});
    Check(introspection.ok, introspection.diagnostics.empty() ? "stdlib introspection failed" : introspection.diagnostics[0].message);
    Commit(state, introspection);
    Lua(state, "assert(Introspection.lookup==rawget and Introspection.assign==rawset and Introspection.next==next and Introspection.setmeta==setmetatable)");
    Reject(state, "local _,backing=pairs(math);backing.abs=nil;Candidate={}", "readonly");

    Reject(state, "Candidate={};while true do end", "instruction limit");
    Reject(state, "Candidate={};pcall(function() while true do end end)");
    Reject(state, "Candidate={blob=string.rep('x',40*1024*1024)}", "heap limit");
    // A fresh batch remains usable after both failed budgets.
    auto recovery = CheckedStage(state, {{"Recovery", "recovery.lua", "Recovery={value=3}"}});
    Check(recovery.ok, "stage budget failure poisoned next isolated state");
    std::vector<Source> many;
    for (int i = 0; i < 5; ++i) {
        const auto name = "Budget" + std::to_string(i);
        many.push_back({name, name + ".lua", "local x=0;for i=1,120000 do x=x+1 end;" + name + "={value=x}"});
    }
    Check(CheckedStage(state, {many[0]}).ok, "individual instruction workload exceeds expected budget");
    auto shared_budget = CheckedStage(state, many);
    Check(!shared_budget.ok && shared_budget.exports.empty() && shared_budget.diagnostics[0].message.find("instruction limit") != std::string::npos,
          "instruction quota was reset between candidate files");

    Reject(state, "local iterator=ipairs({1,2});Candidate={value=iterator}", "unknown C closure");
    for (const char *name : {"find", "match", "gmatch", "gsub"}) {
        Reject(state, std::string("string.") + name + "('aaa','a*a*b');Candidate={}");
        Reject(state, std::string("('aaa'):") + name + "('a*a*b');Candidate={}");
    }
    Reject(state, "Candidate={value=math.huge}", "non-finite");
    Reject(state, "Candidate={value=setmetatable({}, {__gc=function() while true do end end})}", "finalizer");
    Reject(state, "Candidate={value=setmetatable({}, {__close=function() while true do end end})}", "finalizer");
    Reject(state, "Candidate={value=setmetatable({}, {__mode='k'})}", "finalizer");
    Reject(state, "local mt={};local value=setmetatable({},mt);mt.__gc=function() while true do end end;Candidate={value=value}", "finalizer");
    Reject(state, "Candidate={value=setmetatable({}, {__index=function() return 1 end})}", "function __index");
    Reject(state, "local t={};Candidate={value=t};for i=1,70 do t.child={};t=t.child end", "depth 64");
    // Maximum valid depth must also fit the live VM's C API stack during Copy.
    auto deep = CheckedStage(state, {{"Deep", "deep.lua", R"(
        Deep={};local current=Deep
        for i=1,62 do current.child={};current=current.child end
        current.value=7
    )"}});
    Check(deep.ok, deep.diagnostics.empty() ? "valid depth-64 graph failed" : deep.diagnostics[0].message);
    Commit(state, deep);
    Lua(state, "local current=Deep;for i=1,62 do current=current.child end;assert(current.value==7)");
    Reject(state, "Candidate={blob=string.rep('x',9*1024*1024)}", "8 MiB");
    Reject(state, "Candidate={};for i=1,51000 do Candidate[tostring(i)]=i end", "100000");
    auto oversized = CheckedStage(state, {{"Large", "large.lua", std::string(1024 * 1024 + 1, ' ')}});
    Check(!oversized.ok && oversized.diagnostics[0].stage == "validate", "source size limit missing");
    std::string exact_source = "Exact={value=1}"; exact_source.resize(1024 * 1024, ' ');
    Check(CheckedStage(state, {{"Exact", "exact.lua", exact_source}}).ok, "exact 1MiB source boundary rejected");
    std::vector<Source> maximum_sources;
    for (int i = 0; i < 64; ++i) {
        const auto name = "Type" + std::to_string(i);
        maximum_sources.push_back({name, name + ".lua", name + "={value=" + std::to_string(i) + "}"});
    }
    auto maximum_batch = CheckedStage(state, maximum_sources);
    Check(maximum_batch.ok && maximum_batch.exports.size() == 64, "64-source boundary rejected");
    std::vector<Source> excessive(65, {"Same", "same.lua", "Same={}"});
    Check(!CheckedStage(state, excessive).ok, "batch size limit missing");
    Check(!CheckedStage(state, {{"Same", "a.lua", "Same={}"}, {"Same", "b.lua", "Same={}"}}).ok, "duplicate selected type accepted");
    auto reused = CheckedStage(state, {{"FirstExport", "a.lua", "FirstExport={};Later={}"}, {"Later", "b.lua", "local old=Later;Later=old"}});
    Check(!reused.ok && reused.exports.empty(), "same-name table must be new for each source");

    Lua(state, "original_abs=math.abs;math.abs=math.floor");
    Reject(state, "Candidate={fn=math.abs}", "identity");
    Lua(state, "math.abs=original_abs");
    auto environment = CheckedStage(state, {{"Environment", "environment.lua", "local env=_G;Environment={read=function() return env.LiveProbe(5) end}"}});
    Check(environment.ok, "explicit _G capture failed"); Commit(state, environment);
    Lua(state, "assert(Environment.read()==105 and Candidate==old_candidate and host_changes==0)");
    Check(lua_gettop(state) == 1, "test live stack was not balanced");
}
} // namespace
int main() {
    try { Run(); std::cout << "ScriptReloadCandidate: isolation, bounded execution, graph/cell transfer and diagnostics passed\n"; return 0; }
    catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
