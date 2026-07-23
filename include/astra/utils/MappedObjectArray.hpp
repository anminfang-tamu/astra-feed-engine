#pragma once

#include "astra/utils/MappedArray.hpp"

#include <cassert>
#include <cstddef>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>

namespace astra::utils {

// Fixed-capacity mmap storage for the small set of hot arrays whose elements
// require real C++ construction. Mapping still happens once at startup; no
// allocator or virtual dispatch is involved in operator[].
template <typename T> class MappedObjectArray final {
public:
  explicit MappedObjectArray(std::size_t count,
                             std::size_t requested_alignment = 0,
                             const char *mapping_name = nullptr,
                             bool prefault = false)
      : storage_(checkedBytes(count), requested_alignment, mapping_name),
        count_(count) {
    static_assert(std::is_default_constructible_v<T>,
                  "MappedObjectArray elements must be default constructible");
    if (storage_.alignment() % alignof(T) != 0)
      throw std::invalid_argument(
          "MappedObjectArray mapping does not satisfy element alignment");

    if (count_ != 0) {
      T *const storage_begin = reinterpret_cast<T *>(storage_.data());
      // Create an actual T[count_] array, not merely adjacent individual T
      // objects. This gives later indexing and destruction defined array
      // provenance while keeping construction outside the feed path.
      data_ = ::new (static_cast<void *>(storage_begin)) T[count_];
      constructed_ = count_;
      if (data_ != storage_begin) {
        destroyConstructed();
        data_ = nullptr;
        throw std::runtime_error(
            "MappedObjectArray placement array added unexpected overhead");
      }
    }

    if (prefault)
      storage_.prefault();
  }

  ~MappedObjectArray() { destroyConstructed(); }

  MappedObjectArray(const MappedObjectArray &) = delete;
  MappedObjectArray &operator=(const MappedObjectArray &) = delete;
  MappedObjectArray(MappedObjectArray &&) = delete;
  MappedObjectArray &operator=(MappedObjectArray &&) = delete;

  T &operator[](std::size_t index) noexcept {
    assert(index < count_);
    return data_[index];
  }

  const T &operator[](std::size_t index) const noexcept {
    assert(index < count_);
    return data_[index];
  }

  T *data() noexcept { return data_; }
  const T *data() const noexcept { return data_; }
  std::size_t size() const noexcept { return count_; }
  std::size_t bytes() const noexcept { return storage_.bytes(); }
  std::size_t mappedBytes() const noexcept {
    return storage_.mappedBytes();
  }
  std::size_t alignment() const noexcept { return storage_.alignment(); }

  void prefault() noexcept { storage_.prefault(); }

private:
  void destroyConstructed() noexcept {
    // Match ordinary array destruction order.  This matters for the generic
    // utility even though today's descriptor slots are independent.
    while (constructed_ != 0) {
      --constructed_;
      std::destroy_at(data_ + constructed_);
    }
  }

  static std::size_t checkedBytes(std::size_t count) {
    if (count > std::numeric_limits<std::size_t>::max() / sizeof(T))
      throw std::length_error(
          "MappedObjectArray byte size overflows size_t");
    return count * sizeof(T);
  }

  MappedArray<std::byte> storage_;
  std::size_t count_{0};
  T *data_{nullptr};
  std::size_t constructed_{0};
};

} // namespace astra::utils
