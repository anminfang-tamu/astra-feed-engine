#include "replay/taq/NyseTaqReplaySource.hpp"

#include "astra/protocol/MessageType.hpp"
#include "astra/protocol/OrderSide.hpp"

#include <limits>

namespace {

OrderSide toOrderSide(char side) {
  return side == 'S' ? OrderSide::Sell : OrderSide::Buy;
}

bool hasValidPrice(const MdEvent &event) { return event.price_ticks >= 0; }

MarketDataMessage baseMessage(const MdEvent &event) {
  MarketDataMessage message{};
  message.seq_num = event.channel_seq_no;
  message.exhange_ts_ns = event.ts_event_ns;
  message.symbol_id = event.symbol_id;
  message.order_id = event.order_id;
  message.price = event.price_ticks < 0 ? 0
                                        : static_cast<uint64_t>(event.price_ticks);
  message.qty = event.qty;
  message.side = toOrderSide(event.side);
  return message;
}

} // namespace

NyseTaqReplaySource::NyseTaqReplaySource(const std::string &path,
                                         SymbolTable &symbols,
                                         uint8_t channel_id)
    : reader_(path), parser_(symbols, channel_id) {
  if (!reader_.isOpen()) {
    last_error_ = reader_.lastError();
  }
}

bool NyseTaqReplaySource::next(PacketView &packet) {
  packet = PacketView{};

  while (pending_.empty()) {
    std::string line;
    if (!reader_.getline(line)) {
      last_error_ = reader_.lastError();
      return false;
    }

    ++stats_.lines_read;

    MdEvent event{};
    if (!parser_.parseLine(line, event)) {
      if (parser_.lastLineSkipped()) {
        ++stats_.skipped_lines;
      } else {
        ++stats_.bad_lines;
        last_error_ = "line " + std::to_string(reader_.lineNumber()) + ": " +
                      parser_.lastError();
      }
      continue;
    }

    ++stats_.parsed_events;
    if (!enqueueBookMessages(event)) {
      ++stats_.ignored_events;
    }
  }

  MarketDataMessage message = pending_.front();
  pending_.pop_front();

  const std::size_t size =
      encoder_.encode(&message, buffer_.data(), buffer_.size());
  if (size == 0) {
    last_error_ = "failed to encode replay message";
    return false;
  }

  ++stats_.emitted_messages;
  packet.data = buffer_.data();
  packet.size = size;
  packet.receive_ts_ns = message.exhange_ts_ns;
  return true;
}

bool NyseTaqReplaySource::isOpen() const { return reader_.isOpen(); }

const NyseTaqReplayStats &NyseTaqReplaySource::stats() const { return stats_; }

const std::string &NyseTaqReplaySource::lastError() const {
  return last_error_;
}

bool NyseTaqReplaySource::enqueueBookMessages(const MdEvent &event) {
  if (event.symbol_id == 0) {
    return false;
  }

  switch (event.type) {
  case MdEventType::Add: {
    if (!hasValidPrice(event) || event.qty == 0 || event.side == '\0') {
      return false;
    }
    MarketDataMessage message = baseMessage(event);
    message.type = MessageType::AddOrder;
    return enqueueMessage(message);
  }
  case MdEventType::Modify: {
    if (!hasValidPrice(event) || event.side == '\0') {
      return false;
    }
    MarketDataMessage message = baseMessage(event);
    message.type = MessageType::ModifyOrder;
    return enqueueMessage(message);
  }
  case MdEventType::Delete: {
    MarketDataMessage message = baseMessage(event);
    message.price = 0;
    message.qty = 0;
    message.type = MessageType::DeleteOrder;
    return enqueueMessage(message);
  }
  case MdEventType::Execution: {
    if (event.qty == 0) {
      return false;
    }
    MarketDataMessage message = baseMessage(event);
    message.type = MessageType::Trade;
    return enqueueMessage(message);
  }
  case MdEventType::Replace: {
    if (!hasValidPrice(event) || event.qty == 0 || event.side == '\0' ||
        event.new_order_id == 0) {
      return false;
    }

    MarketDataMessage delete_message = baseMessage(event);
    delete_message.price = 0;
    delete_message.qty = 0;
    delete_message.type = MessageType::DeleteOrder;

    MarketDataMessage add_message = baseMessage(event);
    add_message.order_id = event.new_order_id;
    add_message.type = MessageType::AddOrder;

    return enqueueMessage(delete_message) && enqueueMessage(add_message);
  }
  case MdEventType::Trade:
  case MdEventType::Imbalance:
  case MdEventType::Status:
  case MdEventType::Summary:
  case MdEventType::Unknown:
    return false;
  }

  return false;
}

bool NyseTaqReplaySource::enqueueMessage(const MarketDataMessage &message) {
  MarketDataMessage normalized = message;
  normalized.seq_num = next_wire_seq_num_++;
  pending_.push_back(normalized);
  return true;
}
