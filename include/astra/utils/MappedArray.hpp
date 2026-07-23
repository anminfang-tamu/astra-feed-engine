#pragma once

#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <stdexcept>
#include <sys/mman.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif
#include <system_error>
#include <type_traits>
#include <unistd.h>
#include <utility>

namespace astra::utils {

// Fixed-capacity, anonymous, zero-filled storage for very large hot arrays.
// T must be an implicit-lifetime/trivial type because no per-element
// constructors run.  Mapping and optional prefaulting happen before the feed
// loop; operator[] itself is just direct address arithmetic.
template <typename T> class MappedArray final {
public:
  explicit MappedArray(std::size_t count,
                       std::size_t requested_alignment = 0,
                       const char *mapping_name = nullptr)
      : count_(count), logical_bytes_(checkedBytes(count)),
        alignment_(normalizeAlignment(requested_alignment)),
        mapping_name_(mapping_name) {
    static_assert(std::is_trivial_v<T>,
                  "MappedArray elements must be trivial/implicit-lifetime");
    map();
  }

  ~MappedArray() { unmap(); }

  MappedArray(const MappedArray &) = delete;
  MappedArray &operator=(const MappedArray &) = delete;

  MappedArray(MappedArray &&other) noexcept { moveFrom(other); }

  MappedArray &operator=(MappedArray &&other) noexcept {
    if (this != &other) {
      unmap();
      moveFrom(other);
    }
    return *this;
  }

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
  std::size_t bytes() const noexcept { return logical_bytes_; }
  std::size_t mappedBytes() const noexcept { return mapped_bytes_; }
  std::size_t alignment() const noexcept { return alignment_; }

  // Whole-arena prefaulting includes alignment padding.  This matters for
  // exact huge-page residency: a 2 MiB-aligned VMA must have every page in
  // every mapped 2 MiB extent resident before the feed loop starts.
  void prefault() noexcept { prefaultBytes(0, mapped_bytes_); }

  void prefault(std::size_t first_element,
                std::size_t element_count) noexcept {
    if (first_element >= count_ || element_count == 0)
      return;
    const std::size_t available = count_ - first_element;
    if (element_count > available)
      element_count = available;
    prefaultBytes(first_element * sizeof(T), element_count * sizeof(T));
  }

private:
#if defined(MAP_ANONYMOUS)
  static constexpr int kAnonymousMapFlag = MAP_ANONYMOUS;
#elif defined(MAP_ANON)
  static constexpr int kAnonymousMapFlag = MAP_ANON;
#else
#error "MappedArray requires MAP_ANONYMOUS or MAP_ANON"
#endif

  static std::size_t systemPageSize() {
    const long value = ::sysconf(_SC_PAGESIZE);
    if (value <= 0)
      throw std::runtime_error("unable to determine the system page size");
    return static_cast<std::size_t>(value);
  }

  static bool isPowerOfTwo(std::size_t value) noexcept {
    return value != 0 && (value & (value - 1)) == 0;
  }

  static std::size_t checkedBytes(std::size_t count) {
    if (count > std::numeric_limits<std::size_t>::max() / sizeof(T))
      throw std::length_error("MappedArray byte size overflows size_t");
    return count * sizeof(T);
  }

  static std::size_t roundUp(std::size_t value,
                             std::size_t alignment) {
    if (value > std::numeric_limits<std::size_t>::max() - (alignment - 1))
      throw std::length_error("MappedArray rounded size overflows size_t");
    return (value + alignment - 1) & ~(alignment - 1);
  }

  static std::uintptr_t roundUpAddress(std::uintptr_t value,
                                       std::size_t alignment) noexcept {
    return (value + alignment - 1) &
           ~static_cast<std::uintptr_t>(alignment - 1);
  }

  static std::size_t normalizeAlignment(std::size_t requested) {
    const std::size_t page_size = systemPageSize();
    std::size_t alignment = requested == 0 ? page_size : requested;
    if (alignment < page_size)
      alignment = page_size;
    if (alignment < alignof(T))
      alignment = alignof(T);
    if (!isPowerOfTwo(alignment) || alignment % page_size != 0)
      throw std::invalid_argument(
          "MappedArray alignment must be a page-multiple power of two");
    return alignment;
  }

  void map() {
    if (logical_bytes_ == 0)
      return;

    // Keep both ends of the final VMA on the requested alignment.  For the
    // hot arenas the alignment is 2 MiB, so the mapping contains no partial
    // transparent-huge-page extent at either edge.
    mapped_bytes_ = roundUp(logical_bytes_, alignment_);
    if (mapped_bytes_ >
        std::numeric_limits<std::size_t>::max() - alignment_) {
      throw std::length_error("MappedArray aligned mapping size overflows");
    }
    const std::size_t requested_bytes = mapped_bytes_ + alignment_;

    void *mapping = ::mmap(nullptr, requested_bytes, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | kAnonymousMapFlag, -1, 0);
    if (mapping == MAP_FAILED)
      throw std::bad_alloc();

    const auto raw = reinterpret_cast<std::uintptr_t>(mapping);
    const auto aligned = roundUpAddress(raw, alignment_);
    const std::size_t prefix = static_cast<std::size_t>(aligned - raw);
    const std::size_t suffix = requested_bytes - prefix - mapped_bytes_;

    if (prefix != 0 && ::munmap(mapping, prefix) != 0) {
      const int error = errno;
      (void)::munmap(mapping, requested_bytes);
      throw std::system_error(error, std::generic_category(),
                              "MappedArray prefix munmap failed");
    }
    if (suffix != 0) {
      if (::munmap(reinterpret_cast<void *>(aligned + mapped_bytes_),
                   suffix) != 0) {
        const int error = errno;
        (void)::munmap(reinterpret_cast<void *>(aligned),
                       mapped_bytes_ + suffix);
        throw std::system_error(error, std::generic_category(),
                                "MappedArray suffix munmap failed");
      }
    }

    mapping_base_ = reinterpret_cast<std::byte *>(aligned);
#if defined(__linux__) && !defined(MADV_HUGEPAGE)
    if (mapping_name_ != nullptr) {
      unmap();
      throw std::runtime_error(
          "named hot MappedArray requires MADV_HUGEPAGE build headers");
    }
#endif
#if defined(__linux__) && \
    (!defined(PR_SET_VMA) || !defined(PR_SET_VMA_ANON_NAME))
    if (mapping_name_ != nullptr) {
      unmap();
      throw std::runtime_error(
          "named hot MappedArray requires anonymous-VMA naming build headers");
    }
#endif
#if defined(MADV_HUGEPAGE)
    if (::madvise(mapping_base_, mapped_bytes_, MADV_HUGEPAGE) != 0) {
      const int error = errno;
      unmap();
      throw std::system_error(error, std::generic_category(),
                              "MappedArray MADV_HUGEPAGE failed");
    }
#endif
#if defined(__linux__) && defined(PR_SET_VMA) && \
    defined(PR_SET_VMA_ANON_NAME)
    if (mapping_name_ != nullptr) {
      if (::prctl(PR_SET_VMA, PR_SET_VMA_ANON_NAME,
                  reinterpret_cast<unsigned long>(mapping_base_),
                  static_cast<unsigned long>(mapped_bytes_),
                  reinterpret_cast<unsigned long>(mapping_name_)) != 0) {
        const int error = errno;
        unmap();
        throw std::system_error(error, std::generic_category(),
                                "MappedArray VMA naming failed");
      }
    }
#endif

    // mmap supplies storage but is not one of C++20's standard implicit-
    // lifetime-creation operations. First create a byte array covering the
    // complete rounded mapping, including huge-page padding. The logical T[]
    // is then nested in that byte array. Both non-allocating placement array-
    // new expressions are constant work here; omitted () preserves the
    // already-zero anonymous bytes without an element-by-element pass.
    try {
      std::byte *const bytes =
          ::new (static_cast<void *>(mapping_base_)) std::byte[mapped_bytes_];
      if (bytes != mapping_base_) {
        unmap();
        throw std::runtime_error(
            "MappedArray backing array added unexpected overhead");
      }
      mapping_base_ = bytes;
      T *const expected = reinterpret_cast<T *>(mapping_base_);
      T *const objects = ::new (static_cast<void *>(expected)) T[count_];
      if (objects != expected) {
        unmap();
        throw std::runtime_error(
            "MappedArray logical array added unexpected overhead");
      }
      data_ = objects;
    } catch (...) {
      unmap();
      throw;
    }
  }

  void prefaultBytes(std::size_t first_byte,
                     std::size_t byte_count) noexcept {
    if (mapping_base_ == nullptr || byte_count == 0)
      return;

    const std::size_t page_size =
        static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
    volatile std::byte *memory = mapping_base_;
    const std::size_t end = first_byte + byte_count;
    std::size_t offset = first_byte - (first_byte % page_size);
    for (; offset < end; offset += page_size) {
      const std::byte value = memory[offset];
      memory[offset] = value;
    }
    const std::size_t last = end - 1;
    const std::byte value = memory[last];
    memory[last] = value;
  }

  void unmap() noexcept {
    if (mapping_base_ != nullptr) {
      ::munmap(mapping_base_, mapped_bytes_);
      mapping_base_ = nullptr;
    }
    data_ = nullptr;
  }

  void moveFrom(MappedArray &other) noexcept {
    count_ = other.count_;
    logical_bytes_ = other.logical_bytes_;
    mapped_bytes_ = other.mapped_bytes_;
    alignment_ = other.alignment_;
    mapping_name_ = other.mapping_name_;
    mapping_base_ = other.mapping_base_;
    data_ = other.data_;

    other.count_ = 0;
    other.logical_bytes_ = 0;
    other.mapped_bytes_ = 0;
    other.alignment_ = 0;
    other.mapping_name_ = nullptr;
    other.mapping_base_ = nullptr;
    other.data_ = nullptr;
  }

  std::size_t count_{0};
  std::size_t logical_bytes_{0};
  std::size_t mapped_bytes_{0};
  std::size_t alignment_{0};
  const char *mapping_name_{nullptr};
  std::byte *mapping_base_{nullptr};
  T *data_{nullptr};
};

} // namespace astra::utils
