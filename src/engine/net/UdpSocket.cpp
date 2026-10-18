#include "net/UdpSocket.h"

#include <cstring>

#if defined(__EMSCRIPTEN__)
#define POCKET_NET_NONE 1
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace Net {

std::string Address::ToString() const {
    return std::to_string((ip >> 24) & 255) + "." + std::to_string((ip >> 16) & 255) + "." +
           std::to_string((ip >> 8) & 255) + "." + std::to_string(ip & 255) + ":" + std::to_string(port);
}

#ifdef POCKET_NET_NONE

std::optional<Address> Resolve(const std::string &, std::uint16_t) { return std::nullopt; }
std::optional<std::uint32_t> LocalIPv4ForRoute(const Address &) { return std::nullopt; }
UdpSocket::~UdpSocket() = default;
bool UdpSocket::Open(std::uint16_t) { error_ = "netplay is not available on this platform"; return false; }
void UdpSocket::Close() {}
bool UdpSocket::Send(const Address &, const void *, std::size_t) { return false; }
std::optional<std::vector<std::uint8_t>> UdpSocket::Receive(Address &) { return std::nullopt; }

#else

namespace {
#ifdef _WIN32
using SocketHandle = SOCKET;
bool EnsureStartup() {
    static bool started = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return started;
}
void CloseHandle(long long h) { closesocket(static_cast<SOCKET>(h)); }
std::string LastSocketError() { return "socket error " + std::to_string(WSAGetLastError()); }
bool WouldBlock() {
    const int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAECONNRESET; // ICMP port unreachable surfaces as a reset
}
#else
using SocketHandle = int;
bool EnsureStartup() { return true; }
void CloseHandle(long long h) { ::close(static_cast<int>(h)); }
std::string LastSocketError() { return std::strerror(errno); }
bool WouldBlock() { return errno == EAGAIN || errno == EWOULDBLOCK || errno == ECONNREFUSED; }
#endif

sockaddr_in ToSockaddr(const Address &a) {
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(a.ip);
    sa.sin_port = htons(a.port);
    return sa;
}
} // namespace

std::optional<Address> Resolve(const std::string &host, std::uint16_t port) {
    if (!EnsureStartup()) return std::nullopt;
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo *result = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0 || !result) return std::nullopt;
    Address a;
    a.ip = ntohl(reinterpret_cast<sockaddr_in *>(result->ai_addr)->sin_addr.s_addr);
    a.port = port;
    freeaddrinfo(result);
    return a;
}

std::optional<std::uint32_t> LocalIPv4ForRoute(const Address &to) {
    if (!EnsureStartup() || !to.Valid()) return std::nullopt;
    const SocketHandle s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#ifdef _WIN32
    if (s == INVALID_SOCKET) return std::nullopt;
#else
    if (s < 0) return std::nullopt;
#endif
    sockaddr_in sa = ToSockaddr(to);
    std::optional<std::uint32_t> ip;
    if (::connect(s, reinterpret_cast<sockaddr *>(&sa), sizeof(sa)) == 0) {
        sockaddr_in local{};
        socklen_t len = sizeof(local);
        if (getsockname(s, reinterpret_cast<sockaddr *>(&local), &len) == 0) ip = ntohl(local.sin_addr.s_addr);
    }
    CloseHandle(static_cast<long long>(s));
    return ip;
}

UdpSocket::~UdpSocket() { Close(); }

bool UdpSocket::Open(std::uint16_t port) {
    Close();
    if (!EnsureStartup()) { error_ = "socket startup failed"; return false; }
    const SocketHandle s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#ifdef _WIN32
    if (s == INVALID_SOCKET) { error_ = LastSocketError(); return false; }
    u_long nonblocking = 1;
    ioctlsocket(s, FIONBIO, &nonblocking);
#else
    if (s < 0) { error_ = LastSocketError(); return false; }
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons(port);
    if (::bind(s, reinterpret_cast<sockaddr *>(&sa), sizeof(sa)) != 0) {
        error_ = LastSocketError();
        CloseHandle(static_cast<long long>(s));
        return false;
    }
    socklen_t len = sizeof(sa);
    getsockname(s, reinterpret_cast<sockaddr *>(&sa), &len);
    local_port_ = ntohs(sa.sin_port);
    handle_ = static_cast<long long>(s);
    return true;
}

void UdpSocket::Close() {
    if (handle_ >= 0) CloseHandle(handle_);
    handle_ = -1;
    local_port_ = 0;
}

bool UdpSocket::Send(const Address &to, const void *data, std::size_t size) {
    if (handle_ < 0 || !to.Valid()) return false;
    const sockaddr_in sa = ToSockaddr(to);
    const auto sent = ::sendto(static_cast<SocketHandle>(handle_), static_cast<const char *>(data), static_cast<int>(size), 0,
                               reinterpret_cast<const sockaddr *>(&sa), sizeof(sa));
    return sent == static_cast<decltype(sent)>(size);
}

std::optional<std::vector<std::uint8_t>> UdpSocket::Receive(Address &from) {
    if (handle_ < 0) return std::nullopt;
    for (;;) {
        std::uint8_t buffer[2048];
        sockaddr_in sa{};
        socklen_t len = sizeof(sa);
        const auto got = ::recvfrom(static_cast<SocketHandle>(handle_), reinterpret_cast<char *>(buffer), sizeof(buffer), 0,
                                    reinterpret_cast<sockaddr *>(&sa), &len);
        if (got < 0) {
            if (WouldBlock()) {
#ifdef _WIN32
                if (WSAGetLastError() == WSAECONNRESET) continue;
#else
                if (errno == ECONNREFUSED) continue;
#endif
                return std::nullopt;
            }
            error_ = LastSocketError();
            return std::nullopt;
        }
        from.ip = ntohl(sa.sin_addr.s_addr);
        from.port = ntohs(sa.sin_port);
        return std::vector<std::uint8_t>(buffer, buffer + got);
    }
}

#endif

std::optional<Address> ParseAddress(const std::string &text, std::uint16_t default_port) {
    std::string host = text;
    std::uint16_t port = default_port;
    const auto colon = text.rfind(':');
    if (colon != std::string::npos) {
        host = text.substr(0, colon);
        try {
            const int p = std::stoi(text.substr(colon + 1));
            if (p <= 0 || p > 65535) return std::nullopt;
            port = static_cast<std::uint16_t>(p);
        } catch (...) {
            return std::nullopt;
        }
    }
    if (host.empty()) return std::nullopt;
    return Resolve(host, port);
}

} // namespace Net
