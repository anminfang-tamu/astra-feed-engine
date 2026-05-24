#pragma once

#include <cstdint>

struct DecodeStageTiming {
  std::uint64_t mold_parse_ns{0};
  std::uint64_t itch_total_ns{0};
  std::uint64_t book_ns{0};
  std::uint64_t messages{0};
  std::uint64_t book_messages{0};
  std::uint64_t skipped_messages{0};

  void clear() noexcept { *this = {}; }
};
