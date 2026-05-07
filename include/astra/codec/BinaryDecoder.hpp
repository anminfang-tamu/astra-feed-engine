#pragma once

#include <cstddef>

#include "DecodeResult.hpp"

class BinaryDecoder {
public:
  DecodeResult decode(const std::byte *data, std::size_t size);
};