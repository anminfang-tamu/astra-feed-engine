#pragma once

#include "astra/codec/DecodeResult.hpp"
#include "astra/source/PacketView.hpp"

class IPacketProcessor {
public:
  virtual ~IPacketProcessor() = default;
  virtual DecodeResult processPacket(const PacketView &packet) = 0;
};
