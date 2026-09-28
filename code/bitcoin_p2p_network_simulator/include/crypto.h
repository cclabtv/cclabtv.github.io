#ifndef CRYPTO_H
#define CRYPTO_H

#include <array>
#include <cstdint>
#include <vector>
#include <cstring>

inline uint64_t simple_hash(const std::vector<uint8_t> &data)
{
  // FNV-1a inspired
  const uint64_t FNV_PRIME = 0x100000001b3ULL;
  const uint64_t FNV_OFFSET = 0xcbf29ce484222325ULL;

  uint64_t hash = FNV_OFFSET;

  for (uint8_t byte : data)
  {
    hash ^= static_cast<uint64_t>(byte);
    hash *= FNV_PRIME;
  }

  return hash;
}
class SipHashState
{
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
class SipHasher
{
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

/** Hash functor for address using SipHash */
class CServiceHash
{
public:
  CServiceHash() : m_salt_k0{0}, m_salt_k1{0} {}

  CServiceHash(uint64_t salt_k0, uint64_t salt_k1)
      : m_salt_k0{salt_k0}, m_salt_k1{salt_k1} {}

  // Hash uint32_t address
  uint64_t operator()(uint32_t addr) const noexcept
  {
    SipHasher hasher(m_salt_k0, m_salt_k1);
    hasher.write(addr);
    return hasher.finalize();
  }

private:
  const uint64_t m_salt_k0;
  const uint64_t m_salt_k1;
};

#endif // CRYPTO_H