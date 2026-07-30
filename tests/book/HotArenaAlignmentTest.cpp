#include "astra/book/BookManager.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace {

struct ArenaRange {
  const void *base;
  std::size_t mapped_bytes;
};

TEST(HotArenaAlignmentTest, EveryOrderAndPriceArenaIsWholeTwoMiBExtents) {
  constexpr std::size_t alignment = 2u * 1024u * 1024u;
  BookManager manager({{16, 4, false}, {1, false}});
  const auto &orders = manager.orderTable();
  const auto &prices = manager.priceLevels();

  const std::array arenas{
      ArenaRange{orders.directArenaBase(), orders.directArenaMappedBytes()},
      ArenaRange{orders.fallbackArenaBase(),
                 orders.fallbackArenaMappedBytes()},
      ArenaRange{prices.rootsArenaBase(), prices.rootsArenaMappedBytes()},
      ArenaRange{prices.preparedBooksArenaBase(),
                 prices.preparedBooksArenaMappedBytes()},
      ArenaRange{prices.pageArenaBase(), prices.pageArenaMappedBytes()},
      ArenaRange{prices.pageOwnersArenaBase(),
                 prices.pageOwnersArenaMappedBytes()},
      ArenaRange{prices.pageSummariesArenaBase(),
                 prices.pageSummariesArenaMappedBytes()},
      ArenaRange{prices.pageOccupancyArenaBase(),
                 prices.pageOccupancyArenaMappedBytes()},
      ArenaRange{prices.bookSummariesArenaBase(),
                 prices.bookSummariesArenaMappedBytes()},
      ArenaRange{prices.bookOccupancyArenaBase(),
                 prices.bookOccupancyArenaMappedBytes()},
      ArenaRange{manager.bookDescriptorsArenaBase(),
                 manager.bookDescriptorsArenaMappedBytes()},
  };

  for (const ArenaRange arena : arenas) {
    ASSERT_NE(arena.base, nullptr);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(arena.base) % alignment, 0u);
    EXPECT_NE(arena.mapped_bytes, 0u);
    EXPECT_EQ(arena.mapped_bytes % alignment, 0u);
  }
}

} // namespace
