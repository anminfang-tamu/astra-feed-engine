#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>
#include <sys/mman.h>
#include <type_traits>

template <typename T>
class FixedMmapArray {
public:
  explicit FixedMmapArray(std::size_t count) noexcept
      : count_(count), bytes_(checkedBytes(count)),
        data_(map(count_, bytes_)) {
    static_assert(std::is_trivial_v<T>);
    static_assert(std::is_trivially_copyable_v<T>);
    static_assert(std::is_trivially_destructible_v<T>);
  }

  ~FixedMmapArray() { unmap(); }

  FixedMmapArray(const FixedMmapArray &) = delete;
  FixedMmapArray &operator=(const FixedMmapArray &) = delete;

  FixedMmapArray(FixedMmapArray &&other) noexcept { moveFrom(other); }

  FixedMmapArray &operator=(FixedMmapArray &&other) noexcept {
    if (this != &other) {
      unmap();
      moveFrom(other);
    }
    return *this;
  }

  T &operator[](std::size_t idx) noexcept {
    assert(idx < count_);
    return data_[idx];
  }

  const T &operator[](std::size_t idx) const noexcept {
    assert(idx < count_);
    return data_[idx];
  }

  T *data() noexcept { return data_; }
  const T *data() const noexcept { return data_; }

  std::size_t size() const noexcept { return count_; }

private:
#if defined(MAP_ANONYMOUS)
  static constexpr int kAnonymousMapFlag = MAP_ANONYMOUS;
#elif defined(MAP_ANON)
  static constexpr int kAnonymousMapFlag = MAP_ANON;
#else
#error "FixedMmapArray requires MAP_ANONYMOUS or MAP_ANON"
#endif

  static std::size_t checkedBytes(std::size_t count) noexcept {
    if (count > std::numeric_limits<std::size_t>::max() / sizeof(T))
      std::abort();
    return count * sizeof(T);
  }

  static T *map(std::size_t count, std::size_t bytes) noexcept {
    if (bytes == 0) return nullptr;
    void *mapping = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | kAnonymousMapFlag, -1, 0);
    if (mapping == MAP_FAILED) std::abort();
    if (reinterpret_cast<std::uintptr_t>(mapping) % alignof(T) != 0) {
      (void)::munmap(mapping, bytes);
      std::abort();
    }

    // mmap provides suitably aligned storage, but it does not itself begin a
    // C++ T[] lifetime. Non-allocating placement array-new establishes one
    // array spanning the mapping. T is trivial, so omitted () preserves the
    // kernel's zero-filled pages without an element-by-element constructor.
    T *const objects = ::new (mapping) T[count];
    if (objects != mapping) {
      (void)::munmap(mapping, bytes);
      std::abort();
    }
    return objects;
  }

  void unmap() noexcept {
    if (data_ != nullptr) {
      ::munmap(data_, bytes_);
      data_ = nullptr;
    }
  }

  void moveFrom(FixedMmapArray &other) noexcept {
    count_ = other.count_;
    bytes_ = other.bytes_;
    data_ = other.data_;
    other.count_ = 0;
    other.bytes_ = 0;
    other.data_ = nullptr;
  }

  std::size_t count_{0};
  std::size_t bytes_{0};
  T *data_{nullptr};
};
