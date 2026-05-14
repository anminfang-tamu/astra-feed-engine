#pragma once

#include <array>
#include <bit>
#include <cassert>
#include <cstdint>
#include <limits>

class PriceBitmap {
public:
  static constexpr uint32_t kNumBits = 1u << 16;      // 65536 price slots
  static constexpr uint32_t kL0Words = kNumBits / 64; // 1024
  static constexpr uint32_t kL1Words = kL0Words / 64; // 16
  static constexpr uint32_t kL2Words = 1;             // 1 (16 bits used)
  static constexpr uint32_t kInvalid = std::numeric_limits<uint32_t>::max();

  PriceBitmap() noexcept {
    l0_.fill(0);
    l1_.fill(0);
    l2_ = 0;
  }

  // O(1) — three writes
  void set(uint32_t idx) noexcept {
    assert(idx < kNumBits);
    const uint32_t w0 = idx >> 6; // / 64
    const uint32_t b0 = idx & 63;
    const uint32_t w1 = w0 >> 6;
    const uint32_t b1 = w0 & 63;
    const uint32_t b2 = w1; // kL1Words == 16, fits in one word

    l0_[w0] |= (uint64_t{1} << b0);
    l1_[w1] |= (uint64_t{1} << b1);
    l2_ |= (uint64_t{1} << b2);
  }

  // O(1) — clears the L0 bit, and propagates upward only if its word emptied
  void reset(uint32_t idx) noexcept {
    assert(idx < kNumBits);
    const uint32_t w0 = idx >> 6;
    const uint32_t b0 = idx & 63;

    l0_[w0] &= ~(uint64_t{1} << b0);
    if (l0_[w0] != 0)
      return; // common case — done

    const uint32_t w1 = w0 >> 6;
    const uint32_t b1 = w0 & 63;
    l1_[w1] &= ~(uint64_t{1} << b1);
    if (l1_[w1] != 0)
      return;

    l2_ &= ~(uint64_t{1} << w1);
  }

  bool test(uint32_t idx) const noexcept {
    return (l0_[idx >> 6] >> (idx & 63)) & 1u;
  }

  bool empty() const noexcept { return l2_ == 0; }

  // Highest set bit ≤ idx. For bids: best bid =
  // findHighestAtOrBelow(kNumBits-1). Returns kInvalid if none. Worst case: ~6
  // bitops + 3 loads.
  uint32_t findHighestAtOrBelow(uint32_t idx) const noexcept {
    if (idx >= kNumBits)
      idx = kNumBits - 1;

    // Try the L0 word containing idx, masked to bits ≤ idx
    const uint32_t w0 = idx >> 6;
    const uint32_t b0 = idx & 63;
    const uint64_t mask0 = maskAtOrBelow(b0);
    uint64_t bits = l0_[w0] & mask0;
    if (bits)
      return (w0 << 6) | bitHigh(bits);

    // Search L1 for a lower word that's non-empty
    const uint32_t w1 = w0 >> 6;
    const uint32_t b1 = w0 & 63;
    // bits in l1_[w1] strictly below b1
    uint64_t l1bits = l1_[w1] & maskBelow(b1);
    if (l1bits) {
      const uint32_t prevW0 = (w1 << 6) | bitHigh(l1bits);
      return (prevW0 << 6) | bitHigh(l0_[prevW0]);
    }

    // Search L2 for a lower l1 group
    uint64_t l2bits = l2_ & maskBelow(w1);
    if (!l2bits)
      return kInvalid;
    const uint32_t prevW1 = bitHigh(l2bits);
    const uint32_t prevW0 = (prevW1 << 6) | bitHigh(l1_[prevW1]);
    return (prevW0 << 6) | bitHigh(l0_[prevW0]);
  }

  // Lowest set bit ≥ idx. For asks: best ask = findLowestAtOrAbove(0).
  uint32_t findLowestAtOrAbove(uint32_t idx) const noexcept {
    if (idx >= kNumBits)
      return kInvalid;

    const uint32_t w0 = idx >> 6;
    const uint32_t b0 = idx & 63;
    const uint64_t mask0 = maskAtOrAbove(b0);
    uint64_t bits = l0_[w0] & mask0;
    if (bits)
      return (w0 << 6) | bitLow(bits);

    const uint32_t w1 = w0 >> 6;
    const uint32_t b1 = w0 & 63;
    uint64_t l1bits = l1_[w1] & maskAbove(b1);
    if (l1bits) {
      const uint32_t nextW0 = (w1 << 6) | bitLow(l1bits);
      return (nextW0 << 6) | bitLow(l0_[nextW0]);
    }

    uint64_t l2bits = l2_ & maskAbove(w1);
    if (!l2bits)
      return kInvalid;
    const uint32_t nextW1 = bitLow(l2bits);
    const uint32_t nextW0 = (nextW1 << 6) | bitLow(l1_[nextW1]);
    return (nextW0 << 6) | bitLow(l0_[nextW0]);
  }

private:
  // Bit-twiddling helpers — all branchless given valid inputs.
  static uint32_t bitHigh(uint64_t x) noexcept {
    return 63u - static_cast<uint32_t>(std::countl_zero(x));
  }
  static uint32_t bitLow(uint64_t x) noexcept {
    return static_cast<uint32_t>(std::countr_zero(x));
  }
  // Bits [0..b] inclusive. b in [0,63].
  static uint64_t maskAtOrBelow(uint32_t b) noexcept {
    return (~uint64_t{0}) >> (63u - b);
  }
  // Bits [0..b) exclusive. b in [0,63].
  static uint64_t maskBelow(uint32_t b) noexcept {
    return b == 0 ? uint64_t{0} : ((~uint64_t{0}) >> (64u - b));
  }
  // Bits [b..63] inclusive. b in [0,63].
  static uint64_t maskAtOrAbove(uint32_t b) noexcept {
    return (~uint64_t{0}) << b;
  }
  // Bits (b..63] exclusive. b in [0,63].
  static uint64_t maskAbove(uint32_t b) noexcept {
    return b == 63 ? uint64_t{0} : ((~uint64_t{0}) << (b + 1));
  }

  alignas(64) std::array<uint64_t, kL0Words> l0_{};
  alignas(64) std::array<uint64_t, kL1Words> l1_{};
  uint64_t l2_{0};
};