#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace astra::protocol {

constexpr std::array<std::uint8_t, 256> makeItchMessageLengths() noexcept {
  std::array<std::uint8_t, 256> lengths{};
  lengths[static_cast<std::uint8_t>('S')] = 12;
  lengths[static_cast<std::uint8_t>('R')] = 39;
  lengths[static_cast<std::uint8_t>('H')] = 25;
  lengths[static_cast<std::uint8_t>('Y')] = 20;
  lengths[static_cast<std::uint8_t>('L')] = 26;
  lengths[static_cast<std::uint8_t>('V')] = 35;
  lengths[static_cast<std::uint8_t>('W')] = 12;
  lengths[static_cast<std::uint8_t>('K')] = 28;
  lengths[static_cast<std::uint8_t>('J')] = 35;
  lengths[static_cast<std::uint8_t>('h')] = 21;
  lengths[static_cast<std::uint8_t>('A')] = 36;
  lengths[static_cast<std::uint8_t>('F')] = 40;
  lengths[static_cast<std::uint8_t>('E')] = 31;
  lengths[static_cast<std::uint8_t>('C')] = 36;
  lengths[static_cast<std::uint8_t>('X')] = 23;
  lengths[static_cast<std::uint8_t>('D')] = 19;
  lengths[static_cast<std::uint8_t>('U')] = 35;
  lengths[static_cast<std::uint8_t>('P')] = 44;
  lengths[static_cast<std::uint8_t>('Q')] = 40;
  lengths[static_cast<std::uint8_t>('B')] = 19;
  lengths[static_cast<std::uint8_t>('I')] = 50;
  lengths[static_cast<std::uint8_t>('N')] = 20;
  lengths[static_cast<std::uint8_t>('O')] = 48;
  return lengths;
}

inline constexpr auto kItchMessageLengths = makeItchMessageLengths();

constexpr std::uint8_t
expectedItchMessageLength(std::uint8_t type) noexcept {
  return kItchMessageLengths[type];
}

constexpr std::uint8_t expectedItchMessageLength(std::byte type) noexcept {
  return expectedItchMessageLength(std::to_integer<std::uint8_t>(type));
}

} // namespace astra::protocol
