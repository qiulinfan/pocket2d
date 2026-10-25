// Netplay pieces without a network: connection codes, STUN messages, the
// Lua rollback snapshots (in-place restore, upvalues, static tables, hashes)
// and the rollback sound ledger.
#include "audio/SoundLedger.h"
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

// Sounds across corrections: each heard once, late ones played, mispredicted ones stopped.
void Sounds() {
    SoundLedger ledger;
    auto tick = [&](std::uint32_t f, bool resim) { ledger.BeginTick(f, resim); };
    // Ticks 0..4 run on a prediction: tick 1 plays "swing" twice, tick 3 a "whiff".
    for (std::uint32_t f = 0; f < 5; ++f) {
        tick(f, false);
        if (f == 1) {
            const auto a = ledger.Request("swing");
            ledger.SetVoice(101);
            const auto b = ledger.Request("swing");
            ledger.SetVoice(102);
            Check(a.play && b.play && a.nth == 0 && b.nth == 1 && a.late == 0, "first runs play every request");
        }
        if (f == 3) { Check(ledger.Request("whiff").play, "a predicted sound plays"); ledger.SetVoice(103); }
    }
    // The peer's real input changes ticks 2..4: tick 3 hits instead of whiffing.
    ledger.BeginCorrection(1, 5);
    for (std::uint32_t f = 1; f < 5; ++f) {
        tick(f, true);
        if (f == 1) {
            Check(!ledger.Request("swing").play && !ledger.Request("swing").play, "heard sounds are not repeated");
        }
        if (f == 3) {
            const auto hit = ledger.Request("hit");
            Check(hit.play && hit.frame == 3 && hit.late == 2, "a sound first seen in a correction plays late");
            ledger.SetVoice(104);
        }
    }
    const auto cancelled = ledger.EndCorrection();
    Check(cancelled.size() == 1 && cancelled[0].clip == "whiff" && cancelled[0].voice == 103 && cancelled[0].frame == 3,
          "the mispredicted sound is handed back to be stopped");
    // A second correction over the same ticks keeps both swings and the hit.
    tick(5, false);
    ledger.BeginCorrection(2, 6);
    for (std::uint32_t f = 2; f < 6; ++f) {
        tick(f, true);
        if (f == 3) Check(!ledger.Request("hit").play, "a late sound is not played again");
    }
    Check(ledger.EndCorrection().empty() && ledger.Size() == 3, "nothing else changed");
    // A third request in the same tick is a new sound.
    ledger.BeginCorrection(1, 6);
    tick(1, true);
    ledger.Request("swing");
    ledger.Request("swing");
    Check(ledger.Request("swing").play, "a further request of the clip in the tick is new");
    for (std::uint32_t f = 2; f < 6; ++f) { tick(f, true); if (f == 3) ledger.Request("hit"); }
    Check(ledger.EndCorrection().empty() && ledger.Size() == 4, "the extra swing is kept");
    // Old ticks are forgotten; a new session starts afresh.
    tick(200, false);
    Check(ledger.Size() == 0, "requests older than any correction are forgotten");
    tick(0, false);
    Check(ledger.Request("swing").play && ledger.Size() == 1, "a new session starts with an empty ledger");
}

int main() {
    Codes();
    Stun();
    Snapshots();
    Sounds();
    std::puts("netplay_unit_test passed");
    return 0;
}
