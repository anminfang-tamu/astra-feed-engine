#pragma once

#include "astra/channel/ChannelHealth.hpp"
#include "astra/sequencing/GapBuffer.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

struct ChannelState {
  static constexpr std::size_t kSessionBytes = 10;

  uint16_t channel_id{0};
  char session[kSessionBytes + 1]{};

  uint64_t next_expected_seq = 1;
  ChannelHealth status = ChannelHealth::Good;

  // out-of-order packets waiting for missing seqs
  GapBuffer gap_buffer;

  void setSession(const char *session_bytes) noexcept {
    if (session_bytes == nullptr) {
      session[0] = '\0';
      return;
    }
    std::memcpy(session, session_bytes, kSessionBytes);
    session[kSessionBytes] = '\0';
  }

};
