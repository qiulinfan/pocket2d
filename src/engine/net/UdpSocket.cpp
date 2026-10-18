#include "net/UdpSocket.h"

#include <cstring>

#if defined(__EMSCRIPTEN__)
#define POCKET_NET_NONE 1
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
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

namespace {
// Virtual LANs by their address ranges and adapter names.
std::string InterfaceKind(const std::string &name, std::uint32_t ip) {
    std::string lower;
    for (char c : name) lower += static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    if ((ip & 0xFFC00000u) == 0x64400000u || lower.find("tailscale") != std::string::npos) return "tailscale"; // 100.64.0.0/10
    if (lower.find("zerotier") != std::string::npos || lower.rfind("zt", 0) == 0 || lower.rfind("feth", 0) == 0) return "zerotier";
    if ((ip >> 24) == 26u || lower.find("radmin") != std::string::npos) return "radmin"; // Radmin VPN: 26.0.0.0/8
    return "lan";
}
} // namespace

#ifdef POCKET_NET_NONE

std::optional<Address> Resolve(const std::string &, std::uint16_t) { return std::nullopt; }
std::optional<std::uint32_t> LocalIPv4ForRoute(const Address &) { return std::nullopt; }
std::vector<Interface> LocalInterfaces() { return {}; }
std::string HostName() { return "web"; }
UdpSocket::~UdpSocket() = default;
bool UdpSocket::Open(std::uint16_t, bool) { error_ = "netplay is not available on this platform"; return false; }
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

std::vector<Interface> LocalInterfaces() {
    std::vector<Interface> out;
#ifdef _WIN32
    if (!EnsureStartup()) return out;
    ULONG size = 16384;
    std::vector<unsigned char> buffer(size);
    auto *list = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data());
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG result = GetAdaptersAddresses(AF_INET, flags, nullptr, list, &size);
    if (result == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(size);
        list = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data());
        result = GetAdaptersAddresses(AF_INET, flags, nullptr, list, &size);
    }
    if (result != NO_ERROR) return out;
    for (auto *a = list; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        std::string name;
        for (const wchar_t *w = a->FriendlyName; w && *w; ++w) name += *w < 128 ? static_cast<char>(*w) : '?';
        for (auto *u = a->FirstUnicastAddress; u; u = u->Next) {
            if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
            const std::uint32_t ip = ntohl(reinterpret_cast<sockaddr_in *>(u->Address.lpSockaddr)->sin_addr.s_addr);
            const int prefix = u->OnLinkPrefixLength;
            const std::uint32_t mask = prefix <= 0 ? 0 : (prefix >= 32 ? 0xFFFFFFFFu : ~((1u << (32 - prefix)) - 1));
            out.push_back({name, ip, ip | ~mask, InterfaceKind(name, ip)});
        }
    }
#else
    ifaddrs *list = nullptr;
    if (getifaddrs(&list) != 0) return out;
    for (ifaddrs *a = list; a; a = a->ifa_next) {
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET) continue;
        if (!(a->ifa_flags & IFF_UP) || (a->ifa_flags & IFF_LOOPBACK)) continue;
        const std::uint32_t ip = ntohl(reinterpret_cast<sockaddr_in *>(a->ifa_addr)->sin_addr.s_addr);
        std::uint32_t broadcast = ip;
        if (a->ifa_netmask) broadcast = ip | ~ntohl(reinterpret_cast<sockaddr_in *>(a->ifa_netmask)->sin_addr.s_addr);
        out.push_back({a->ifa_name ? a->ifa_name : "", ip, broadcast, InterfaceKind(a->ifa_name ? a->ifa_name : "", ip)});
    }
    freeifaddrs(list);
#endif
    return out;
}

std::string HostName() {
    if (!EnsureStartup()) return "player";
    char name[256] = {};
    if (gethostname(name, sizeof(name) - 1) != 0 || !name[0]) return "player";
    std::string out = name;
    const auto dot = out.find(".local");
    if (dot != std::string::npos) out.resize(dot);
    return out;
}

UdpSocket::~UdpSocket() { Close(); }

bool UdpSocket::Open(std::uint16_t port, bool shared) {
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
    const int on = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char *>(&on), sizeof(on));
    if (shared) {
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&on), sizeof(on));
#ifdef SO_REUSEPORT
        setsockopt(s, SOL_SOCKET, SO_REUSEPORT, reinterpret_cast<const char *>(&on), sizeof(on));
#endif
    }
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
