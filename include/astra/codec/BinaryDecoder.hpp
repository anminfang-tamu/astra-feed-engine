#pragma once

#include <cstddef>

#include "DecodeResult.hpp"
#include "astra/protocol/MarketDataMessageView.hpp"
#include "astra/source/PacketView.hpp"

class BinaryDecoder {
public:
  DecodeResult decode(const PacketView &packet, MarketDataMessageView &message);
};