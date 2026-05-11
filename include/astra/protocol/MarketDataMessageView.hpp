#pragma once

#include "astra/protocol/MessageType.hpp"
#include "astra/protocol/OrderSide.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

class MarketDataMessageView {
public:
  MarketDataMessageView() = default;

  explicit MarketDataMessageView(const std::byte *data) : data_(data) {}

  uint64_t seqNum() const { return load<uint64_t>(0); }
  uint64_t exchangeTsNs() const { return load<uint64_t>(8); }
  uint64_t orderId() const { return load<uint64_t>(16); }
  uint32_t symbolId() const { return load<uint32_t>(24); }
  uint32_t price() const { return load<uint32_t>(28); }
  uint32_t qty() const { return load<uint32_t>(32); }

  MessageType type() const {
    return static_cast<MessageType>(load<uint8_t>(36));
  }

  OrderSide side() const { return static_cast<OrderSide>(load<uint8_t>(37)); }

private:
  template <typename T> T load(std::size_t offset) const {
    T value;
    std::memcpy(&value, data_ + offset, sizeof(T));
    return value;
  }

  const std::byte *data_{nullptr};
};
