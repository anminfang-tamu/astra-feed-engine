#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

class OrderRefDirectory {
public:
  static constexpr uint32_t kInvalidIdx = std::numeric_limits<uint32_t>::max();
  // Production default: one pre-touched direct slot for every uint32 order ref.
  static constexpr uint64_t kDefaultMaxDirectOrderRef =
      std::numeric_limits<uint32_t>::max();

  struct Handle {
    uint16_t stock_locate{0};
    uint32_t pool_index{kInvalidIdx};

    bool valid() const noexcept {
      return stock_locate != 0 && pool_index != kInvalidIdx;
    }
  };

  explicit OrderRefDirectory(
      uint64_t max_direct_order_ref = kDefaultMaxDirectOrderRef)
      : max_direct_order_ref_(max_direct_order_ref),
        direct_entries_(entryCount(max_direct_order_ref), kEmpty) {
    touchPages();
  }

  bool canStore(uint64_t order_ref) const noexcept {
    return order_ref <= max_direct_order_ref_;
  }

  bool insert(uint64_t order_ref, uint16_t stock_locate,
              uint32_t pool_index) noexcept {
    if (!canStore(order_ref) || stock_locate == 0 ||
        pool_index == kInvalidIdx) {
      return false;
    }

    uint64_t &entry = direct_entries_[directIndex(order_ref)];
    if (entry != kEmpty) {
      return false;
    }

    entry = pack(stock_locate, pool_index);
    return true;
  }

  Handle find(uint64_t order_ref) const noexcept {
    if (!canStore(order_ref)) {
      return {};
    }

    return unpack(direct_entries_[directIndex(order_ref)]);
  }

  void erase(uint64_t order_ref) noexcept {
    if (!canStore(order_ref)) {
      return;
    }

    direct_entries_[directIndex(order_ref)] = kEmpty;
  }

private:
  static constexpr uint64_t kEmpty = 0;

  static size_t entryCount(uint64_t max_direct_order_ref) noexcept {
    return static_cast<size_t>(max_direct_order_ref + 1);
  }

  static size_t directIndex(uint64_t order_ref) noexcept {
    return static_cast<size_t>(order_ref);
  }

  static uint64_t pack(uint16_t stock_locate, uint32_t pool_index) noexcept {
    return (static_cast<uint64_t>(stock_locate) << 32) |
           static_cast<uint64_t>(pool_index + 1);
  }

  static Handle unpack(uint64_t entry) noexcept {
    if (entry == kEmpty) {
      return {};
    }

    return {static_cast<uint16_t>(entry >> 32),
            static_cast<uint32_t>((entry & 0xffffffffULL) - 1)};
  }

  void touchPages() noexcept {
    static constexpr size_t kPageSize = 4096;
    static constexpr size_t kEntriesPerPage = kPageSize / sizeof(uint64_t);

    if (direct_entries_.empty()) {
      return;
    }

    volatile uint64_t *entries = direct_entries_.data();
    for (size_t index = 0; index < direct_entries_.size();
         index += kEntriesPerPage) {
      entries[index] = entries[index];
    }
    entries[direct_entries_.size() - 1] = entries[direct_entries_.size() - 1];
  }

  uint64_t max_direct_order_ref_;
  std::vector<uint64_t> direct_entries_;
};
