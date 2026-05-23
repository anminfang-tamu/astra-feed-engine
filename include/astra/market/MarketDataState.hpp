#pragma once

#include "astra/book/BookManager.hpp"
#include "astra/channel/ChannelState.hpp"

#include <array>

struct MarketDataState {
  static constexpr std::size_t kMaxChannels = 32;

  std::array<ChannelState, kMaxChannels> channels{};
  BookManager books;
};
