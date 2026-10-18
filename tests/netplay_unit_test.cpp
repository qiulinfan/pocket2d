// Netplay pieces without a network: connection codes, STUN messages and the
// Lua rollback snapshots (in-place restore, upvalues, static tables, hashes).
#include "net/ConnectionCode.h"
#include "scripting/APIRegistration.h"
#include "lua.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {
void Check(bool condition, const char *message) {
    if (condition) return;
    std::fprintf(stderr, "netplay_unit_test: %s\n", message);
    std::exit(1);
}

void Lua(lua_State *state, const char *code) {
    if (luaL_dostring(state, code) == LUA_OK) return;
    std::fprintf(stderr, "netplay_unit_test: %s\n", lua_tostring(state, -1));
    std::exit(1);
}

void Codes() {
    Net::PeerCode code;
    code.public_address = Net::Address{0xCB007105u, 51234}; // 203.0.113.5
    code.lan_address = Net::Address{0xC0A8010Au, 51234};    // 192.168.1.10
    const std::string text = Net::EncodeCode(code);
    Check(text.size() == 23 && text[7] == '-' && text[15] == '-', "a code is three groups of seven");
    const auto back = Net::DecodeCode(text);
    Check(back && back->public_address == code.public_address && back->lan_address == code.lan_address, "codes round-trip");
    std::string loose;
    for (char c : text) loose += c == '-' ? ' ' : static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    Check(Net::DecodeCode(loose).has_value(), "lower case and spaces are accepted");
    std::string typo = text;
    typo[3] = typo[3] == 'A' ? 'B' : 'A';
    Check(!Net::DecodeCode(typo).has_value(), "a typo fails the check byte");
    Check(!Net::DecodeCode("ABC").has_value(), "a short code is rejected");
}

void Stun() {
    Net::StunTransaction t{};
    for (std::size_t i = 0; i < t.size(); ++i) t[i] = static_cast<std::uint8_t>(i * 7 + 1);
    const auto request = Net::StunRequest(t);
    Check(request.size() == 20 && Net::IsStunPacket(request.data(), request.size()), "a binding request");
    const Net::Address mapped{0xCB007105u, 40000};
    const auto response = Net::StunResponse(t, mapped);
    const auto parsed = Net::ParseStunResponse(response.data(), response.size(), t);
    Check(parsed && *parsed == mapped, "the XOR-mapped address is read back");
    Net::StunTransaction other = t;
    other[0] ^= 1;
    Check(!Net::ParseStunResponse(response.data(), response.size(), other), "another transaction's answer is ignored");
}

void Snapshots() {
    lua_State *state = luaL_newstate();
    luaL_openlibs(state);
    APIRegistration::Initialize(state);
    Lua(state, R"(
        DATA = {big = {1, 2, 3}}
        Net.Static(DATA)
        local counter = 0
        function Bump() counter = counter + 1; return counter end
        state = {x = 1, list = {10, 20}, child = {name = "a"}}
        alias = state.child
        Net.Track(_G)
        Net.Snapshot(0, "save")
        local h0 = Net.Snapshot(0, "hash")
        state.x = 2
        state.list[3] = 30
        state.child.name = "b"
        state.extra = {}
        Bump(); Bump()
        DATA.big[1] = 99
        Net.Snapshot(1, "save")
        assert(Net.Snapshot(1, "hash") ~= h0, "a changed state hashes differently")
        Net.Snapshot(0, "load")
        assert(state.x == 1 and #state.list == 2 and state.extra == nil, "contents restored")
        assert(alias == state.child and alias.name == "a", "tables keep their identity")
        assert(Bump() == 1, "upvalues restored")
        assert(DATA.big[1] == 99, "static tables are not restored")
        Net.Snapshot(2, "save")
        assert(Net.Snapshot(2, "hash") ~= h0, "the upvalue moved on")
        Net.Snapshot(0, "load")
        Net.Snapshot(3, "save")
        assert(Net.Snapshot(3, "hash") == h0, "the same state hashes the same")
        Net.Snapshot(1, "load")
        assert(state.x == 2 and state.list[3] == 30 and state.extra ~= nil and state.child.name == "b", "a later slot")
    )");
    lua_close(state);
}
} // namespace

int main() {
    Codes();
    Stun();
    Snapshots();
    std::puts("netplay_unit_test passed");
    return 0;
}
