#include "replay/itch/ItchReplaySource.hpp"

#include "astra/protocol/MessageType.hpp"
#include "astra/protocol/OrderSide.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <ios>
#include <span>
#include <string>

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
  message.price =
      event.price_ticks < 0 ? 0 : static_cast<uint64_t>(event.price_ticks);
  message.qty = event.qty;
  message.side = toOrderSide(event.side);
  return message;
}

uint16_t decodeLength(const std::array<unsigned char, 2> &bytes) {
  return static_cast<uint16_t>((static_cast<uint16_t>(bytes[0]) << 8) |
                               static_cast<uint16_t>(bytes[1]));
}

} // namespace

ItchReplaySource::ItchReplaySource(const std::string &path,
                                   SymbolTable &symbols, uint8_t channel_id)
    : input_(path, std::ios::binary), parser_(symbols, channel_id) {
  if (!input_.is_open()) {
    last_error_ = "failed to open ITCH file: " + path;
  }
  message_buffer_.reserve(64);
}

bool ItchReplaySource::next(PacketView &packet) {
  packet = PacketView{};

  while (pending_.empty()) {
    if (!readNextMessage()) {
      return false;
    }

    MdEvent event{};
    if (!parser_.parseMessage(
            std::span<const std::byte>(message_buffer_.data(),
                                       message_buffer_.size()),
            event)) {
      if (parser_.lastMessageSkipped()) {
        ++stats_.skipped_messages;
      } else {
        ++stats_.bad_messages;
        last_error_ = "ITCH record " + std::to_string(stats_.records_read) +
                      ": " + parser_.lastError();
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
    last_error_ = "failed to encode ITCH replay message";
    return false;
  }

  ++stats_.emitted_messages;
  packet.data = buffer_.data();
  packet.size = size;
  packet.receive_ts_ns = message.exhange_ts_ns;
  return true;
}

bool ItchReplaySource::isOpen() const { return input_.is_open(); }

const ItchReplayStats &ItchReplaySource::stats() const { return stats_; }

const std::string &ItchReplaySource::lastError() const { return last_error_; }

bool ItchReplaySource::readNextMessage() {
  std::array<unsigned char, 2> length_bytes{};
  input_.read(reinterpret_cast<char *>(length_bytes.data()),
              static_cast<std::streamsize>(length_bytes.size()));

  if (input_.gcount() == 0 && input_.eof()) {
    return false;
  }

  if (input_.gcount() != static_cast<std::streamsize>(length_bytes.size())) {
    ++stats_.bad_messages;
    last_error_ = "truncated ITCH message length";
    return false;
  }

  const uint16_t message_size = decodeLength(length_bytes);
  if (message_size == 0) {
    ++stats_.bad_messages;
    last_error_ = "zero-length ITCH message";
    return false;
  }

  message_buffer_.resize(message_size);
  input_.read(reinterpret_cast<char *>(message_buffer_.data()),
              static_cast<std::streamsize>(message_buffer_.size()));
  if (input_.gcount() != static_cast<std::streamsize>(message_buffer_.size())) {
    ++stats_.bad_messages;
    last_error_ = "truncated ITCH message payload";
    return false;
  }

  ++stats_.records_read;
  stats_.bytes_read += static_cast<uint64_t>(message_size) + 2;
  return true;
}

bool ItchReplaySource::enqueueBookMessages(const MdEvent &event) {
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

bool ItchReplaySource::enqueueMessage(const MarketDataMessage &message) {
  MarketDataMessage normalized = message;
  normalized.seq_num = next_wire_seq_num_++;
  pending_.push_back(normalized);
  return true;
}
