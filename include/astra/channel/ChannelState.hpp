#pragma once

#include "astra/channel/ChannelHealth.hpp"
#include "astra/sequencing/GapBuffer.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

struct ChannelState {
  static constexpr std::size_t kSessionBytes = 10;
  static constexpr std::size_t kLineCount = 2;

  uint16_t channel_id{0};
  char session[kSessionBytes + 1]{};
  bool session_initialized{false};

  uint64_t next_expected_seq = 1;
  // Zero means no packet has been observed. MoldUDP64 sequenced data starts at
  // sequence 1, so zero is available as the observability sentinel.
  uint64_t first_received_seq{0};
  std::array<uint64_t, kLineCount> first_received_seq_by_line{};
  uint64_t session_mismatch_packets{0};
  ChannelHealth status = ChannelHealth::Good;

  // out-of-order packets waiting for missing seqs
  GapBuffer gap_buffer;

  void setSession(const char *session_bytes) noexcept {
    if (session_bytes == nullptr) {
      session[0] = '\0';
      session_initialized = false;
      return;
    }
    std::memcpy(session, session_bytes, kSessionBytes);
    session[kSessionBytes] = '\0';
    session_initialized = true;
  }

  bool sessionMatches(const char *session_bytes) const noexcept {
    return session_initialized && session_bytes != nullptr &&
           std::memcmp(session, session_bytes, kSessionBytes) == 0;
  }
};
