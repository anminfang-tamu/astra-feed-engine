#include "astra/codec/BinaryDecoder.hpp"
#include "astra/protocol/MarketDataMessageView.hpp"
#include "astra/protocol/MessageType.hpp"
#include "astra/protocol/WireMessage.hpp"

DecodeResult BinaryDecoder::decode(const PacketView &packet,
                                   MarketDataMessageView &message) {
  if (packet.data == nullptr || packet.size < WireMessageSize) {
    return {DecodeStatus::PacketTooSmall};
  }

  MarketDataMessageView view(packet.data);

  const auto type = view.type();
  if (type < MessageType::AddOrder || type > MessageType::SnapshotEnd) {
    return {DecodeStatus::InvalidMessageType};
  }

  const auto side = view.side();
  if (side != OrderSide::Buy && side != OrderSide::Sell) {
    return {DecodeStatus::InvalidOrderSide};
  }

  message = view;
  return {DecodeStatus::Ok};
};