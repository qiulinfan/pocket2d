#ifndef POCKET2D_NET_LINK_H
#define POCKET2D_NET_LINK_H

#include "net/UdpSocket.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

// The link brings two peers to a datagram channel (Netplay.cpp runs the
// session over it). Peer packets start with "PKN1" and a type byte; the link
// handles punches itself and hands every other packet to the session.
namespace Netplay::detail {

constexpr std::uint8_t kMagic[4] = {'P', 'K', 'N', '1'};
enum PacketType : std::uint8_t {
    kPunch = 1, kPunchAck = 2, kStart = 3, kStartAck = 4, kInput = 5, kBye = 6,
    kResync = 7, kResyncAck = 8, kResyncAsk = 9, // desync recovery (Netplay.cpp)
};
constexpr std::uint16_t kServerPort = 27777;
constexpr std::uint16_t kDirectPort = 27778;

std::uint64_t NowMs();
std::vector<std::uint8_t> Header(PacketType type);
void PutU32(std::vector<std::uint8_t> &b, std::uint32_t v);
std::uint32_t GetU32(const std::uint8_t *p);

enum class LinkState {
    Opening,    // contacting the room server or the STUN servers
    Waiting,    // a host without a peer yet
    Connecting, // punching to a known peer
    Connected,  // datagrams reach the peer (directly or relayed)
    Failed,
};

// What a direct host tells LAN discovery.
struct DiscoveryInfo {
    int port = 27779;
    std::string game, name, payload;
};

class Link {
public:
    virtual ~Link() = default;
    // Receives and handles link traffic; session packets go to `received`.
    virtual void Pump(std::uint64_t now, std::vector<std::vector<std::uint8_t>> &received) = 0;
    virtual void Send(const std::vector<std::uint8_t> &packet) = 0;
    // A timed punch: its answer is a round-trip sample (and counts for the loss).
    virtual void Probe() = 0;
    // Room server links: also time the relay while connecting (relay_rtt),
    // and switch to it (UseRelay) when it is the faster path.
    virtual bool CanRelay() const { return false; }
    virtual void UseRelay() {}
    // Connection codes: the peer's code (the host gets it after creating its own).
    virtual bool ConnectCode(const std::string &code) = 0;
    // Tells the room server the room is closed.
    virtual void Close() = 0;

    LinkState state = LinkState::Opening;
    std::string kind;             // "server", "direct", "code"
    std::string error, room, code, nat = "unknown", peer_text, lan_text;
    bool relay = false;
    int best_rtt = -1;           // the path in use
    int rtt_samples = 0;
    int direct_rtt = -1, relay_rtt = -1; // best round trip of each path (-1: not measured)
    int relay_samples = 0;
    bool probe_relay = false;    // room server: time the relay path too (until the session starts)
    std::uint64_t last_recv = 0;

    // Loss: the share of recent probes on the path in use that got no answer.
    void NoteProbe(std::uint32_t stamp);
    void NoteAnswer(std::uint32_t stamp);
    double Loss(std::uint64_t now) const; // -1 until enough probes are old enough

protected:
    std::deque<std::pair<std::uint32_t, bool>> probes_; // stamp, answered
};

// Each returns a link, or nullptr with the reason in `error`.
std::unique_ptr<Link> ServerLink(const std::string &server, int side, const std::string &room, bool force_relay,
                                 std::string &error);
std::unique_ptr<Link> DirectHostLink(int port, const DiscoveryInfo &discovery, std::string &error);
std::unique_ptr<Link> DirectJoinLink(const std::string &address, std::string &error);
std::unique_ptr<Link> CodeLink(int side, const std::vector<std::string> &stun, const std::string &host_code,
                               std::string &error);

} // namespace Netplay::detail

#endif
