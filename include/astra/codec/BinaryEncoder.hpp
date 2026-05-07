#pragma once

#include <cstddef>

class BinaryEncoder {
public:
  std::size_t encode(const void *message, std::byte *buffer,
                     std::size_t buffer_size);
};