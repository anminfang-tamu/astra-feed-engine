#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <vector>

class OrderRefDirectory {
public:
  static constexpr uint32_t kInvalidIdx = std::numeric_limits<uint32_t>::max();
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
        direct_pages_(pageCount(max_direct_order_ref)) {}

  bool canStore(uint64_t order_ref) const noexcept {
    return order_ref <= max_direct_order_ref_;
  }

  bool insert(uint64_t order_ref, uint16_t stock_locate,
              uint32_t pool_index) noexcept {
    if (!canStore(order_ref) || stock_locate == 0 ||
        pool_index == kInvalidIdx) {
      return false;
    }

    Page *page = ensurePage(pageIndex(order_ref));
    if (page == nullptr) {
      return false;
    }

    uint64_t &entry = page->entries[offset(order_ref)];
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

    const Page *page = pageFor(pageIndex(order_ref));
    if (page == nullptr) {
      return {};
    }

    return unpack(page->entries[offset(order_ref)]);
  }

  void erase(uint64_t order_ref) noexcept {
    if (!canStore(order_ref)) {
      return;
    }

    Page *page = pageFor(pageIndex(order_ref));
    if (page == nullptr) {
      return;
    }

    page->entries[offset(order_ref)] = kEmpty;
  }

private:
  static constexpr uint64_t kEmpty = 0;
  static constexpr uint64_t kPageBits = 16;
  static constexpr uint64_t kPageSize = 1ULL << kPageBits;
  static constexpr uint64_t kPageMask = kPageSize - 1;

  struct Page {
    Page() noexcept { entries.fill(kEmpty); }
    std::array<uint64_t, kPageSize> entries;
  };

  static size_t pageCount(uint64_t max_direct_order_ref) noexcept {
    return static_cast<size_t>((max_direct_order_ref >> kPageBits) + 1);
  }

  static size_t pageIndex(uint64_t order_ref) noexcept {
    return static_cast<size_t>(order_ref >> kPageBits);
  }

  static size_t offset(uint64_t order_ref) noexcept {
    return static_cast<size_t>(order_ref & kPageMask);
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

  Page *pageFor(size_t index) noexcept {
    if (index >= direct_pages_.size()) {
      return nullptr;
    }
    return direct_pages_[index].get();
  }

  const Page *pageFor(size_t index) const noexcept {
    if (index >= direct_pages_.size()) {
      return nullptr;
    }
    return direct_pages_[index].get();
  }

  Page *ensurePage(size_t index) noexcept {
    if (index >= direct_pages_.size()) {
      return nullptr;
    }

    if (direct_pages_[index] == nullptr) {
      Page *page = new (std::nothrow) Page();
      if (page == nullptr) {
        return nullptr;
      }
      direct_pages_[index].reset(page);
    }

    return direct_pages_[index].get();
  }

  uint64_t max_direct_order_ref_;
  std::vector<std::unique_ptr<Page>> direct_pages_;
};
