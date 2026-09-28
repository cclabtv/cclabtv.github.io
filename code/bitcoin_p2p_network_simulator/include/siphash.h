// Copyright (c) 2016-present The Bitcoin Core developers
// Adapted for Bitcoin P2P Network Simulator
// Distributed under the MIT software license

#ifndef SIPHASH_H
#define SIPHASH_H

#include <array>
#include <cstdint>
#include <vector>

/** Shared SipHash internal state v[0..3], initialized from (k0, k1). */
class SipHashState {
    static constexpr uint64_t C0{0x736f6d6570736575ULL};
    static constexpr uint64_t C1{0x646f72616e646f6dULL};
    static constexpr uint64_t C2{0x6c7967656e657261ULL};
    static constexpr uint64_t C3{0x7465646279746573ULL};

public:
    explicit SipHashState(uint64_t k0, uint64_t k1) noexcept
        : v{C0 ^ k0, C1 ^ k1, C2 ^ k0, C3 ^ k1} {}

    std::array<uint64_t, 4> v{};
};

/** General SipHash-2-4 implementation. */
class SipHasher {
    SipHashState m_state;
    uint64_t m_tmp{0};
    uint8_t m_count{0};

public:
    /** Construct a SipHash calculator initialized with 128-bit key (k0, k1). */
    SipHasher(uint64_t k0, uint64_t k1);

    /** Hash a 64-bit integer worth of data. */
    SipHasher &write(uint64_t data);

    /** Hash arbitrary bytes. */
    SipHasher &write(const std::vector<uint8_t> &data);

    /** Compute the 64-bit SipHash-2-4 of the data written so far. */
    uint64_t finalize() const;
};

#endif // SIPHASH_H
