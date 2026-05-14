#pragma once

#include "astra/codec/BinaryEncoder.hpp"
#include "astra/protocol/MarketDataMessage.hpp"
#include "astra/protocol/WireMessage.hpp"
#include "astra/source/IMarketDataSource.hpp"
#include "replay/GzipLineReader.hpp"
#include "replay/MdEvent.hpp"
#include "replay/taq/NyseTaqParser.hpp"
#include "replay/SymbolTable.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>

struct NyseTaqReplayStats {
  uint64_t lines_read{0};
  uint64_t parsed_events{0};
  uint64_t skipped_lines{0};
  uint64_t bad_lines{0};
  uint64_t ignored_events{0};
  uint64_t emitted_messages{0};
};

class NyseTaqReplaySource : public IMarketDataSource {
public:
  NyseTaqReplaySource(const std::string &path, SymbolTable &symbols,
                      uint8_t channel_id = 0);

  bool next(PacketView &packet) override;

  bool isOpen() const;
  const NyseTaqReplayStats &stats() const;
  const std::string &lastError() const;

private:
  bool enqueueBookMessages(const MdEvent &event);
  bool enqueueMessage(const MarketDataMessage &message);

  GzipLineReader reader_;
  NyseTaqParser parser_;
  BinaryEncoder encoder_;
  NyseTaqReplayStats stats_;
  std::string last_error_;
  std::deque<MarketDataMessage> pending_;
  std::array<std::byte, WireMessageSize> buffer_{};
  uint64_t next_wire_seq_num_{1};
};
