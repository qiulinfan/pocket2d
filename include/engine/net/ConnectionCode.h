#ifndef POCKET2D_CONNECTION_CODE_H
#define POCKET2D_CONNECTION_CODE_H

#include "net/UdpSocket.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Connection codes: how two players reach each other without a server of
// their own. Each side learns its public address from a public STUN server and
// shows it, with its LAN address, as a short code the other side types in.
namespace Net {

struct PeerCode {
    Address public_address; // as a STUN server saw this socket
    Address lan_address;    // this machine on its own network
};

// 21 Crockford base32 characters in three groups ("ABCDEFG-HJKMNPQ-RSTVWXY"),
// with a check byte so typos are caught.
std::string EncodeCode(const PeerCode &code);
// Accepts lower case, spaces and dashes, and O/I/L for 0/1/1.
std::optional<PeerCode> DecodeCode(const std::string &text);

// STUN (RFC 5389) Binding: the request and the mapped address in a response.
using StunTransaction = std::array<std::uint8_t, 12>;
std::vector<std::uint8_t> StunRequest(const StunTransaction &transaction);
bool IsStunPacket(const std::uint8_t *data, std::size_t size);
std::optional<Address> ParseStunResponse(const std::uint8_t *data, std::size_t size, const StunTransaction &transaction);
// A Binding success response (test servers).
std::vector<std::uint8_t> StunResponse(const StunTransaction &transaction, const Address &mapped);

} // namespace Net

#endif
