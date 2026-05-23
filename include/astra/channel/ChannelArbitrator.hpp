#pragma once

#include "astra/channel/ChannelHealth.hpp"
#include "astra/channel/ChannelState.hpp"

class ChannelArbitrator {
public:
  static ChannelHealth
  determine_channel_health(const ChannelState &channel_state) noexcept {
    return channel_state.status;
  }
};
