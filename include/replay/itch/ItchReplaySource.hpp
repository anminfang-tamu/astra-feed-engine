#pragma once

#include "astra/codec/BinaryEncoder.hpp"
#include "astra/protocol/MarketDataMessage.hpp"
#include "astra/protocol/WireMessage.hpp"
#include "astra/source/IMarketDataSource.hpp"
#include "replay/MdEvent.hpp"
#include "replay/SymbolTable.hpp"
#include "replay/itch/ItchParser.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <fstream>
#include <string>
#include <vector>

struct ItchReplayStats {
  uint64_t records_read{0};
  uint64_t bytes_read{0};
  uint64_t parsed_events{0};
  uint64_t skipped_messages{0};
  uint64_t bad_messages{0};
  uint64_t ignored_events{0};
  uint64_t emitted_messages{0};
};

class ItchReplaySource : public IMarketDataSource {
public:
  ItchReplaySource(const std::string &path, SymbolTable &symbols,
                   uint8_t channel_id = 0);

  bool next(PacketView &packet) override;

  bool isOpen() const;
  const ItchReplayStats &stats() const;
  const std::string &lastError() const;

private:
  bool readNextMessage();
  bool enqueueBookMessages(const MdEvent &event);
  bool enqueueMessage(const MarketDataMessage &message);

  std::ifstream input_;
  ItchParser parser_;
  BinaryEncoder encoder_;
  ItchReplayStats stats_;
  std::string last_error_;
  std::deque<MarketDataMessage> pending_;
  std::vector<std::byte> message_buffer_;
  std::array<std::byte, WireMessageSize> buffer_{};
  uint64_t next_wire_seq_num_{1};
};
