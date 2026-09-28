// Copyright (c) 2016-present The Bitcoin Core developers
// Adapted for Bitcoin P2P Network Simulator
// Distributed under the MIT software license

#include "siphash.h"
#include <cstring>

static void halfsiphash_state_rounds(std::array<uint64_t, 4> &v, int rounds) {
    for (int i = 0; i < rounds; ++i) {
        v[0] += v[1];
        v[1] = ((v[1] << 13) | (v[1] >> (64 - 13))) ^ v[0];
        v[0] = ((v[0] << 32) | (v[0] >> (64 - 32)));

        v[2] += v[3];
        v[3] = ((v[3] << 16) | (v[3] >> (64 - 16))) ^ v[2];

        v[0] += v[3];
        v[3] = ((v[3] << 21) | (v[3] >> (64 - 21))) ^ v[0];

        v[2] += v[1];
        v[1] = ((v[1] << 17) | (v[1] >> (64 - 17))) ^ v[2];
        v[2] = ((v[2] << 32) | (v[2] >> (64 - 32)));
    }
}

SipHasher::SipHasher(uint64_t k0, uint64_t k1) : m_state(k0, k1) {}

SipHasher &SipHasher::write(uint64_t data) {
    uint64_t v = data;
    m_tmp |= (uint64_t)((uint8_t)(v & 0xFF)) << (8 * m_count);
    v >>= 8;
    for (int i = 1; i < 8; ++i) {
        m_tmp |= (uint64_t)((uint8_t)(v & 0xFF)) << (8 * (m_count + i));
        v >>= 8;
    }
    m_count += 8;
    if (m_count == 8) {
        m_state.v[3] ^= m_tmp;
        halfsiphash_state_rounds(m_state.v, 2);
        m_state.v[0] ^= m_tmp;
        m_tmp = 0;
        m_count = 0;
    }
    return *this;
}

SipHasher &SipHasher::write(const std::vector<uint8_t> &data) {
    uint8_t tmp[8] = {};
    int count = 0;

    for (const uint8_t &b : data) {
        tmp[count % 8] = b;
        if (++count % 8 == 0) {
            uint64_t v;
            std::memcpy(&v, tmp, 8);
            m_state.v[3] ^= v;
            halfsiphash_state_rounds(m_state.v, 2);
            m_state.v[0] ^= v;
        }
    }

    if (count % 8 != 0) {
        uint64_t v = 0;
        std::memcpy(&v, tmp, count % 8);
        m_tmp = v;
        m_count = count % 8;
    }

    return *this;
}

uint64_t SipHasher::finalize() const {
    std::array<uint64_t, 4> v = m_state.v;
    uint64_t b = ((uint64_t)m_count) << 56 | m_tmp;
    v[3] ^= b;
    halfsiphash_state_rounds(v, 2);
    v[0] ^= b;
    v[2] ^= 0xff;
    halfsiphash_state_rounds(v, 4);
    return v[1] ^ v[3];
}
