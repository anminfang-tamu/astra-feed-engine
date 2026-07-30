#include "astra/utils/MappedObjectArray.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace {

struct TrackedObject {
  static inline std::uint32_t live = 0;
  static inline std::uint32_t destroyed = 0;

  TrackedObject() noexcept : value(41) { ++live; }
  ~TrackedObject() {
    --live;
    ++destroyed;
  }

  std::uint32_t value;
};

struct ThrowingObject {
  static inline std::uint32_t attempts = 0;
  static inline std::uint32_t live = 0;

  ThrowingObject() {
    if (attempts++ == 3)
      throw std::runtime_error("injected constructor failure");
    ++live;
  }
  ~ThrowingObject() { --live; }
};

TEST(MappedObjectArrayTest, ConstructsIndexesAndDestroysEveryObject) {
  constexpr std::size_t alignment = 2u * 1024u * 1024u;
  TrackedObject::live = 0;
  TrackedObject::destroyed = 0;

  {
    astra::utils::MappedObjectArray<TrackedObject> objects(7, alignment);
    ASSERT_NE(objects.data(), nullptr);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(objects.data()) % alignment,
              0u);
    EXPECT_EQ(objects.mappedBytes(), alignment);
    EXPECT_EQ(TrackedObject::live, 7u);
    EXPECT_EQ(objects[4].value, 41u);
    objects[4].value = 99;
    EXPECT_EQ(objects[4].value, 99u);
  }

  EXPECT_EQ(TrackedObject::live, 0u);
  EXPECT_EQ(TrackedObject::destroyed, 7u);
}

TEST(MappedObjectArrayTest, ConstructorFailureDestroysCompletedPrefix) {
  constexpr std::size_t alignment = 2u * 1024u * 1024u;
  ThrowingObject::attempts = 0;
  ThrowingObject::live = 0;

  EXPECT_THROW((astra::utils::MappedObjectArray<ThrowingObject>(
                   8, alignment)),
               std::runtime_error);
  EXPECT_EQ(ThrowingObject::live, 0u);
}

} // namespace
