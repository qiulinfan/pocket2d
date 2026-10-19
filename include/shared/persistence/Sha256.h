#ifndef POCKET2D_SHA256_H
#define POCKET2D_SHA256_H

#include <cstddef>
#include <cstdint>
#include <string>

// SHA-256 (FIPS 180-4): save checksums and update package digests.
class Sha256 {
public:
    void Add(const void *data, std::size_t size);
    // The digest as 64 lowercase hex digits (ends the hash).
    std::string Hex();
    static std::string Of(const void *data, std::size_t size);

private:
    void Block();
    std::uint32_t h_[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::uint8_t buffer_[64] = {};
    std::size_t used_ = 0;
    std::uint64_t bits_ = 0;
};

#endif
