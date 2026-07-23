#pragma once

#include <array>
#include <bit>
#include <cassert>
#include <cstdint>
#include <limits>
#include <type_traits>

// The storage is deliberately a trivial, all-zero-is-empty aggregate.  That
// lets large collections of bitmaps live in anonymous mmap arenas without
// running millions of constructors.  PriceBitmap below is the owning wrapper
// used by ordinary stack objects; the paged book operates on Storage directly.
struct alignas(64) PriceBitmapStorage {
  static constexpr uint32_t kNumBits = 1u << 16;
  static constexpr uint32_t kL0Words = kNumBits / 64;
  static constexpr uint32_t kL1Words = kL0Words / 64;

  std::array<uint64_t, kL0Words> l0;
  std::array<uint64_t, kL1Words> l1;
  uint64_t l2;
};

static_assert(std::is_trivial_v<PriceBitmapStorage>);
static_assert(std::is_trivially_destructible_v<PriceBitmapStorage>);
static_assert(alignof(PriceBitmapStorage) == 64);

class PriceBitmap {
public:
  using Storage = PriceBitmapStorage;

  struct NoopWorkObserver {
    constexpr void onWordLoad() noexcept {}
  };

  static constexpr uint32_t kNumBits = Storage::kNumBits;
  static constexpr uint32_t kL0Words = Storage::kL0Words;
  static constexpr uint32_t kL1Words = Storage::kL1Words;
  static constexpr uint32_t kL2Words = 1;
  static constexpr uint32_t kInvalid = std::numeric_limits<uint32_t>::max();
  static constexpr uint64_t kValidL2Mask =
      (~uint64_t{0}) >> (64u - kL1Words);

  // A predecessor/successor lookup reads no more than one word at each of the
  // three hierarchy levels, plus the selected child words.
  static constexpr uint32_t kMaxSearchWordLoads = 5;

  PriceBitmap() noexcept = default;

  void set(uint32_t idx) noexcept { set(storage_, idx); }
  void reset(uint32_t idx) noexcept { reset(storage_, idx); }
  bool test(uint32_t idx) const noexcept { return test(storage_, idx); }
  bool empty() const noexcept { return empty(storage_); }
  uint32_t findHighestAtOrBelow(uint32_t idx) const noexcept {
    return findHighestAtOrBelow(storage_, idx);
  }
  uint32_t findLowestAtOrAbove(uint32_t idx) const noexcept {
    return findLowestAtOrAbove(storage_, idx);
  }

  const Storage &storage() const noexcept { return storage_; }

  // O(1): three fixed-address writes.
  static void set(Storage &storage, uint32_t idx) noexcept {
    assert(idx < kNumBits);
    const uint32_t w0 = idx >> 6;
    const uint32_t b0 = idx & 63;
    const uint32_t w1 = w0 >> 6;
    const uint32_t b1 = w0 & 63;

    storage.l0[w0] |= (uint64_t{1} << b0);
    storage.l1[w1] |= (uint64_t{1} << b1);
    storage.l2 |= (uint64_t{1} << w1);
  }

  // O(1): one mandatory clear and at most two summary clears.
  static void reset(Storage &storage, uint32_t idx) noexcept {
    assert(idx < kNumBits);
    const uint32_t w0 = idx >> 6;
    const uint32_t b0 = idx & 63;

    storage.l0[w0] &= ~(uint64_t{1} << b0);
    if (storage.l0[w0] != 0)
      return;

    const uint32_t w1 = w0 >> 6;
    const uint32_t b1 = w0 & 63;
    storage.l1[w1] &= ~(uint64_t{1} << b1);
    if (storage.l1[w1] != 0)
      return;

    storage.l2 &= ~(uint64_t{1} << w1);
  }

  static bool test(const Storage &storage, uint32_t idx) noexcept {
    assert(idx < kNumBits);
    return ((storage.l0[idx >> 6] >> (idx & 63)) & 1u) != 0;
  }

  static bool empty(const Storage &storage) noexcept {
    return (storage.l2 & kValidL2Mask) == 0;
  }

  // Highest set bit <= idx.  The search has three fixed hierarchy levels and
  // never scans through a run of empty prices.
  static uint32_t findHighestAtOrBelow(const Storage &storage,
                                       uint32_t idx) noexcept {
    NoopWorkObserver observer;
    return findHighestAtOrBelowObserved(storage, idx, observer);
  }

  // A counting observer can be instantiated by tests without adding a counter
  // or branch to production. Both paths execute this same search body; the
  // no-op observer is eliminated by optimized builds.
  template <typename WorkObserver>
  static uint32_t findHighestAtOrBelowObserved(
      const Storage &storage, uint32_t idx,
      WorkObserver &observer) noexcept {
    if (idx >= kNumBits)
      idx = kNumBits - 1;

    const uint32_t w0 = idx >> 6;
    const uint32_t b0 = idx & 63;
    uint64_t bits = loadWord(storage.l0[w0], observer) & maskAtOrBelow(b0);
    if (bits)
      return (w0 << 6) | bitHigh(bits);

    const uint32_t w1 = w0 >> 6;
    const uint32_t b1 = w0 & 63;
    const uint64_t l1bits =
        loadWord(storage.l1[w1], observer) & maskBelow(b1);
    if (l1bits) {
      const uint32_t previous_w0 = (w1 << 6) | bitHigh(l1bits);
      const uint64_t child =
          loadWord(storage.l0[previous_w0], observer);
      return child == 0 ? kInvalid
                        : (previous_w0 << 6) | bitHigh(child);
    }

    const uint64_t l2bits =
        loadWord(storage.l2, observer) & kValidL2Mask & maskBelow(w1);
    if (!l2bits)
      return kInvalid;
    const uint32_t previous_w1 = bitHigh(l2bits);
    const uint64_t l1_child =
        loadWord(storage.l1[previous_w1], observer);
    if (l1_child == 0)
      return kInvalid;
    const uint32_t previous_w0 =
        (previous_w1 << 6) | bitHigh(l1_child);
    const uint64_t l0_child =
        loadWord(storage.l0[previous_w0], observer);
    return l0_child == 0
               ? kInvalid
               : (previous_w0 << 6) | bitHigh(l0_child);
  }

  // Lowest set bit >= idx.  Like the predecessor lookup, this is bounded by
  // the fixed three-level hierarchy.
  static uint32_t findLowestAtOrAbove(const Storage &storage,
                                      uint32_t idx) noexcept {
    NoopWorkObserver observer;
    return findLowestAtOrAboveObserved(storage, idx, observer);
  }

  template <typename WorkObserver>
  static uint32_t findLowestAtOrAboveObserved(
      const Storage &storage, uint32_t idx,
      WorkObserver &observer) noexcept {
    if (idx >= kNumBits)
      return kInvalid;

    const uint32_t w0 = idx >> 6;
    const uint32_t b0 = idx & 63;
    uint64_t bits = loadWord(storage.l0[w0], observer) & maskAtOrAbove(b0);
    if (bits)
      return (w0 << 6) | bitLow(bits);

    const uint32_t w1 = w0 >> 6;
    const uint32_t b1 = w0 & 63;
    const uint64_t l1bits =
        loadWord(storage.l1[w1], observer) & maskAbove(b1);
    if (l1bits) {
      const uint32_t next_w0 = (w1 << 6) | bitLow(l1bits);
      const uint64_t child = loadWord(storage.l0[next_w0], observer);
      return child == 0 ? kInvalid : (next_w0 << 6) | bitLow(child);
    }

    const uint64_t l2bits =
        loadWord(storage.l2, observer) & kValidL2Mask & maskAbove(w1);
    if (!l2bits)
      return kInvalid;
    const uint32_t next_w1 = bitLow(l2bits);
    const uint64_t l1_child = loadWord(storage.l1[next_w1], observer);
    if (l1_child == 0)
      return kInvalid;
    const uint32_t next_w0 = (next_w1 << 6) | bitLow(l1_child);
    const uint64_t l0_child = loadWord(storage.l0[next_w0], observer);
    return l0_child == 0 ? kInvalid
                         : (next_w0 << 6) | bitLow(l0_child);
  }

private:
  template <typename WorkObserver>
  static uint64_t loadWord(const uint64_t &word,
                           WorkObserver &observer) noexcept {
    observer.onWordLoad();
    return word;
  }

  static uint32_t bitHigh(uint64_t value) noexcept {
    return 63u - static_cast<uint32_t>(std::countl_zero(value));
  }

  static uint32_t bitLow(uint64_t value) noexcept {
    return static_cast<uint32_t>(std::countr_zero(value));
  }

  static uint64_t maskAtOrBelow(uint32_t bit) noexcept {
    return (~uint64_t{0}) >> (63u - bit);
  }

  static uint64_t maskBelow(uint32_t bit) noexcept {
    return bit == 0 ? uint64_t{0}
                    : ((~uint64_t{0}) >> (64u - bit));
  }

  static uint64_t maskAtOrAbove(uint32_t bit) noexcept {
    return (~uint64_t{0}) << bit;
  }

  static uint64_t maskAbove(uint32_t bit) noexcept {
    return bit == 63 ? uint64_t{0}
                     : ((~uint64_t{0}) << (bit + 1));
  }

  Storage storage_{};
};
