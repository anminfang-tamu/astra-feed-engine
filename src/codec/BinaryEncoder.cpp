#include "astra/codec/BinaryEncoder.hpp"
#include "astra/protocol/MarketDataMessage.hpp"
#include "astra/protocol/WireMessage.hpp"

#include <cstring>

std::size_t BinaryEncoder::encode(const void *message, std::byte *buffer,
                                  std::size_t buffer_size) {
  if (message == nullptr || buffer == nullptr || buffer_size < WireMessageSize) {
    return 0;
  }

  const auto *msg = static_cast<const MarketDataMessage *>(message);

  WireMessage wire;
  wire.seq_num       = msg->seq_num;
  wire.exhange_ts_ns = msg->exhange_ts_ns;
  wire.order_id      = msg->order_id;
  wire.symbol_id     = msg->symbol_id;
  wire.price         = msg->price;
  wire.qty           = msg->qty;
  wire.type          = msg->type;
  wire.side          = msg->side;

  std::memcpy(buffer, &wire, WireMessageSize);
  return WireMessageSize;
}