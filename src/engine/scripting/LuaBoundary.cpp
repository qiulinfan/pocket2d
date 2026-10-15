#include "scripting/LuaBoundary.h"
#include <algorithm>
#include <cstddef>
#include <string_view>

namespace {
constexpr std::size_t kDiagnosticLimit=4096;
constexpr std::string_view kTruncated="...[truncated]";
std::size_t Sequence(std::string_view input,std::size_t offset) {
    const auto byte=static_cast<unsigned char>(input[offset]);
    if(byte<0x80)return ((byte>=0x20&&byte!=0x7f)||byte=='\n'||byte=='\r'||byte=='\t')?1:0;
    const std::size_t length=byte>=0xc2&&byte<=0xdf?2:byte>=0xe0&&byte<=0xef?3:byte>=0xf0&&byte<=0xf4?4:0;
    if(!length||length>input.size()-offset)return 0;
    for(std::size_t index=1;index<length;++index)
        if((static_cast<unsigned char>(input[offset+index])&0xc0)!=0x80)return 0;
    const auto second=static_cast<unsigned char>(input[offset+1]);
    if((byte==0xe0&&second<0xa0)||(byte==0xed&&second>=0xa0)||
       (byte==0xf0&&second<0x90)||(byte==0xf4&&second>=0x90))return 0;
    return length;
}
// Fixed storage and trivial locals only: this also runs inside a Lua C message
// handler, where luaL_traceback may longjmp on allocation failure.
std::size_t Sanitize(std::string_view input,char *output,std::size_t capacity) {
    constexpr char hex[]="0123456789ABCDEF";
    const std::size_t budget=capacity-kTruncated.size();
    std::size_t used=0,cursor=0;
    while(cursor<input.size()) {
        const auto length=Sequence(input,cursor);
        if(used+(length?length:4)>budget)break;
        if(length)for(std::size_t index=0;index<length;++index)output[used++]=input[cursor++];
        else {
            const auto byte=static_cast<unsigned char>(input[cursor++]);
            output[used++]='\\';output[used++]='x';output[used++]=hex[byte>>4];output[used++]=hex[byte&15];
        }
    }
    if(cursor<input.size())for(char c:kTruncated)output[used++]=c;
    output[used]='\0';return used;
}
std::string Diagnostic(std::string_view input) {
    char buffer[kDiagnosticLimit+1];const auto size=Sanitize(input,buffer,kDiagnosticLimit);
    return std::string(buffer,size);
}
int Traceback(lua_State *state) {
    std::size_t length=0;
    const char *text=lua_type(state,1)==LUA_TSTRING?lua_tolstring(state,1,&length):nullptr;
    const std::string_view input=text?std::string_view(text,length):std::string_view("non-string Lua error");
    // Leave room in the final bounded diagnostic for traceback/source context.
    char buffer[2049];Sanitize(input,buffer,2048);
    luaL_traceback(state,state,buffer,1);return 1;
}
}
namespace LuaBoundary {
Error::Error(std::string_view message):std::runtime_error(Diagnostic(message)){}

bool IsEnabled(const luabridge::LuaRef &value) {
    auto *state=value.state();const int top=lua_gettop(state);
    struct Restore {lua_State *state;int top;~Restore(){lua_settop(state,top);}} restore{state,top};
    if(!lua_checkstack(state,1))throw Error("Lua enabled value could not reserve stack space");
    value.push(state);
    return lua_type(state,-1)!=LUA_TBOOLEAN||lua_toboolean(state,-1)!=0;
}

void ProtectedCall(lua_State *state,int nargs,int nresults) {
    const int top=lua_gettop(state);
    if(nargs<0||nargs>=top||nresults<LUA_MULTRET)throw Error("invalid protected Lua call shape");
    const int base=top-nargs-1;
    struct RestoreFailure {lua_State *state;int base;bool success=false;~RestoreFailure(){if(!success)lua_settop(state,base);}} restore{state,base};
    if(!lua_checkstack(state,2)) {lua_settop(state,base);throw Error("Lua call could not reserve traceback stack space");}
    lua_pushcfunction(state,Traceback);lua_insert(state,base+1);
    int status;
    try {status=lua_pcall(state,nargs,nresults,base+1);}
    catch(const luabridge::LuaException &failure) {
        const Error error(failure.what());lua_settop(state,base);throw error;
    }
    if(status!=LUA_OK) {
        std::size_t length=0;
        const char *text=lua_type(state,-1)==LUA_TSTRING?lua_tolstring(state,-1,&length):nullptr;
        const Error error(text?std::string_view(text,length):std::string_view("non-string Lua error"));
        lua_settop(state,base);throw error;
    }
    lua_remove(state,base+1);
    restore.success=true;
}
}
