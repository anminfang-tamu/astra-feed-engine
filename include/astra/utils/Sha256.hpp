#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace astra::utils {

class Sha256 final {
public:
  using Digest = std::array<unsigned char, 32>;

  Sha256() noexcept;

  void update(std::span<const unsigned char> input);
  void update(std::string_view input);

  [[nodiscard]] Digest finalize();
  [[nodiscard]] std::string finalizeHex();

private:
  void transform(std::span<const unsigned char, 64> block) noexcept;

  std::array<std::uint32_t, 8> state_{};
  std::array<unsigned char, 64> buffer_{};
  Digest digest_{};
  std::uint64_t total_bytes_{0};
  std::size_t buffered_bytes_{0};
  bool finalized_{false};
};

[[nodiscard]] std::string
sha256Hex(std::span<const unsigned char> input);
[[nodiscard]] std::string sha256Hex(std::string_view input);

} // namespace astra::utils
