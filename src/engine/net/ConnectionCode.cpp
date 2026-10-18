#include "net/ConnectionCode.h"

#include <cstring>

namespace Net {
namespace {

constexpr char kAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"; // Crockford base32
constexpr std::size_t kBytes = 13;                                // 12 address bytes + check
constexpr std::size_t kChars = (kBytes * 8 + 4) / 5;              // 21
constexpr std::uint32_t kCookie = 0x2112A442u;

std::uint8_t Crc8(const std::uint8_t *data, std::size_t size) {
    std::uint8_t crc = 0;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) crc = static_cast<std::uint8_t>((crc & 0x80) ? (crc << 1) ^ 0x07 : crc << 1);
    }
    return crc;
}

int CharValue(char c) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    if (c == 'O') c = '0';
    if (c == 'I' || c == 'L') c = '1';
    for (int i = 0; i < 32; ++i)
        if (kAlphabet[i] == c) return i;
    return -1;
}

void PutAddress(std::uint8_t *p, const Address &a) {
    p[0] = static_cast<std::uint8_t>(a.ip >> 24);
    p[1] = static_cast<std::uint8_t>(a.ip >> 16);
    p[2] = static_cast<std::uint8_t>(a.ip >> 8);
    p[3] = static_cast<std::uint8_t>(a.ip);
    p[4] = static_cast<std::uint8_t>(a.port >> 8);
    p[5] = static_cast<std::uint8_t>(a.port);
}

Address GetAddress(const std::uint8_t *p) {
    Address a;
    a.ip = (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
    a.port = static_cast<std::uint16_t>((p[4] << 8) | p[5]);
    return a;
}

std::uint16_t Get16(const std::uint8_t *p) { return static_cast<std::uint16_t>((p[0] << 8) | p[1]); }
std::uint32_t Get32(const std::uint8_t *p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}

} // namespace

std::string EncodeCode(const PeerCode &code) {
    std::uint8_t bytes[kBytes];
    PutAddress(bytes, code.public_address);
    PutAddress(bytes + 6, code.lan_address);
    bytes[12] = Crc8(bytes, 12);
    std::string out;
    std::uint32_t buffer = 0;
    int bits = 0;
    std::size_t written = 0;
    for (std::size_t i = 0; i < kBytes; ++i) {
        buffer = (buffer << 8) | bytes[i];
        bits += 8;
        while (bits >= 5) {
            bits -= 5;
            if (written && written % 7 == 0) out += '-';
            out += kAlphabet[(buffer >> bits) & 31];
            ++written;
        }
    }
    if (bits > 0) {
        if (written % 7 == 0) out += '-';
        out += kAlphabet[(buffer << (5 - bits)) & 31];
    }
    return out;
}

std::optional<PeerCode> DecodeCode(const std::string &text) {
    std::vector<int> values;
    for (char c : text) {
        if (c == '-' || c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
        const int v = CharValue(c);
        if (v < 0) return std::nullopt;
        values.push_back(v);
    }
    if (values.size() != kChars) return std::nullopt;
    std::uint8_t bytes[kBytes] = {};
    std::uint32_t buffer = 0;
    int bits = 0;
    std::size_t n = 0;
    for (int v : values) {
        buffer = (buffer << 5) | static_cast<std::uint32_t>(v);
        bits += 5;
        if (bits >= 8 && n < kBytes) {
            bits -= 8;
            bytes[n++] = static_cast<std::uint8_t>(buffer >> bits);
        }
    }
    if (n != kBytes || Crc8(bytes, 12) != bytes[12]) return std::nullopt;
    PeerCode code;
    code.public_address = GetAddress(bytes);
    code.lan_address = GetAddress(bytes + 6);
    if (!code.public_address.Valid() && !code.lan_address.Valid()) return std::nullopt;
    return code;
}

std::vector<std::uint8_t> StunRequest(const StunTransaction &transaction) {
    std::vector<std::uint8_t> b = {0x00, 0x01, 0x00, 0x00, 0x21, 0x12, 0xA4, 0x42};
    b.insert(b.end(), transaction.begin(), transaction.end());
    return b;
}

bool IsStunPacket(const std::uint8_t *data, std::size_t size) {
    return size >= 20 && (data[0] & 0xC0) == 0 && Get32(data + 4) == kCookie;
}

std::optional<Address> ParseStunResponse(const std::uint8_t *data, std::size_t size, const StunTransaction &transaction) {
    if (!IsStunPacket(data, size) || Get16(data) != 0x0101) return std::nullopt;
    if (std::memcmp(data + 8, transaction.data(), transaction.size()) != 0) return std::nullopt;
    const std::size_t length = Get16(data + 2);
    if (20 + length > size) return std::nullopt;
    std::optional<Address> mapped;
    for (std::size_t at = 20; at + 4 <= 20 + length;) {
        const std::uint16_t type = Get16(data + at);
        const std::uint16_t len = Get16(data + at + 2);
        const std::uint8_t *v = data + at + 4;
        if (at + 4 + len > 20 + length) break;
        if ((type == 0x0020 || type == 0x0001) && len >= 8 && v[1] == 0x01) { // IPv4
            Address a;
            a.port = Get16(v + 2);
            a.ip = Get32(v + 4);
            if (type == 0x0020) {
                a.port = static_cast<std::uint16_t>(a.port ^ (kCookie >> 16));
                a.ip ^= kCookie;
                return a; // XOR-MAPPED-ADDRESS wins
            }
            mapped = a;
        }
        at += 4 + ((len + 3u) & ~3u);
    }
    return mapped;
}

std::vector<std::uint8_t> StunResponse(const StunTransaction &transaction, const Address &mapped) {
    std::vector<std::uint8_t> b = {0x01, 0x01, 0x00, 0x0C, 0x21, 0x12, 0xA4, 0x42};
    b.insert(b.end(), transaction.begin(), transaction.end());
    const std::uint16_t port = static_cast<std::uint16_t>(mapped.port ^ (kCookie >> 16));
    const std::uint32_t ip = mapped.ip ^ kCookie;
    const std::uint8_t attr[12] = {0x00, 0x20, 0x00, 0x08, 0x00, 0x01,
                                   static_cast<std::uint8_t>(port >> 8), static_cast<std::uint8_t>(port),
                                   static_cast<std::uint8_t>(ip >> 24), static_cast<std::uint8_t>(ip >> 16),
                                   static_cast<std::uint8_t>(ip >> 8), static_cast<std::uint8_t>(ip)};
    b.insert(b.end(), attr, attr + 12);
    return b;
}

} // namespace Net
