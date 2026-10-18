#ifndef POCKET2D_UDP_SOCKET_H
#define POCKET2D_UDP_SOCKET_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// A non-blocking IPv4 UDP socket for netplay. Addresses are host-order IPv4
// numbers with a port. Unavailable on the web build (Open returns false).
namespace Net {

struct Address {
    std::uint32_t ip = 0;   // host order
    std::uint16_t port = 0;
    bool operator==(const Address &other) const { return ip == other.ip && port == other.port; }
    bool operator!=(const Address &other) const { return !(*this == other); }
    bool Valid() const { return ip != 0 && port != 0; }
    std::string ToString() const;
};

// "host" or "1.2.3.4"; blocking DNS lookup.
std::optional<Address> Resolve(const std::string &host, std::uint16_t port);
std::optional<Address> ParseAddress(const std::string &text, std::uint16_t default_port);
// This machine's IPv4 address on the interface that routes to `to` (no packet is sent).
std::optional<std::uint32_t> LocalIPv4ForRoute(const Address &to);

class UdpSocket {
public:
    UdpSocket() = default;
    ~UdpSocket();
    UdpSocket(const UdpSocket &) = delete;
    UdpSocket &operator=(const UdpSocket &) = delete;

    // Binds to all interfaces on `port` (0 = any free port).
    bool Open(std::uint16_t port = 0);
    void Close();
    bool IsOpen() const { return handle_ >= 0; }
    std::uint16_t LocalPort() const { return local_port_; }
    const std::string &LastError() const { return error_; }

    bool Send(const Address &to, const void *data, std::size_t size);
    // One datagram, or nothing when none is waiting.
    std::optional<std::vector<std::uint8_t>> Receive(Address &from);

private:
    long long handle_ = -1;
    std::uint16_t local_port_ = 0;
    std::string error_;
};

} // namespace Net

#endif
