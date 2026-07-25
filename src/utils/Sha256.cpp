#include "astra/utils/Sha256.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace astra::utils {
namespace {

constexpr std::uint32_t rotateRight(std::uint32_t value,
                                    unsigned int bits) noexcept {
  return (value >> bits) | (value << (32u - bits));
}

constexpr std::array<std::uint32_t, 64> kRoundConstants{
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

} // namespace

Sha256::Sha256() noexcept
    : state_{0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
             0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u} {}

void Sha256::transform(
    std::span<const unsigned char, 64> block) noexcept {
  std::array<std::uint32_t, 64> words{};
  for (std::size_t index = 0; index < 16u; ++index) {
    const std::size_t offset = index * 4u;
    words[index] =
        (static_cast<std::uint32_t>(block[offset]) << 24u) |
        (static_cast<std::uint32_t>(block[offset + 1u]) << 16u) |
        (static_cast<std::uint32_t>(block[offset + 2u]) << 8u) |
        static_cast<std::uint32_t>(block[offset + 3u]);
  }
  for (std::size_t index = 16u; index < words.size(); ++index) {
    const std::uint32_t s0 =
        rotateRight(words[index - 15u], 7u) ^
        rotateRight(words[index - 15u], 18u) ^
        (words[index - 15u] >> 3u);
    const std::uint32_t s1 =
        rotateRight(words[index - 2u], 17u) ^
        rotateRight(words[index - 2u], 19u) ^
        (words[index - 2u] >> 10u);
    words[index] =
        words[index - 16u] + s0 + words[index - 7u] + s1;
  }

  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];
  for (std::size_t index = 0; index < words.size(); ++index) {
    const std::uint32_t sum1 =
        rotateRight(e, 6u) ^ rotateRight(e, 11u) ^ rotateRight(e, 25u);
    const std::uint32_t choose = (e & f) ^ (~e & g);
    const std::uint32_t temporary1 =
        h + sum1 + choose + kRoundConstants[index] + words[index];
    const std::uint32_t sum0 =
        rotateRight(a, 2u) ^ rotateRight(a, 13u) ^ rotateRight(a, 22u);
    const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temporary2 = sum0 + majority;
    h = g;
    g = f;
    f = e;
    e = d + temporary1;
    d = c;
    c = b;
    b = a;
    a = temporary1 + temporary2;
  }

  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void Sha256::update(std::span<const unsigned char> input) {
  if (finalized_)
    throw std::logic_error("cannot update a finalized SHA-256");
  constexpr std::uint64_t kMaximumBytes =
      std::numeric_limits<std::uint64_t>::max() / 8u;
  if (input.size() > kMaximumBytes - total_bytes_)
    throw std::length_error("SHA-256 input exceeds 64-bit bit length");
  total_bytes_ += static_cast<std::uint64_t>(input.size());

  if (buffered_bytes_ != 0) {
    const std::size_t copied =
        std::min(input.size(), buffer_.size() - buffered_bytes_);
    std::copy_n(input.begin(), copied, buffer_.begin() + buffered_bytes_);
    buffered_bytes_ += copied;
    input = input.subspan(copied);
    if (buffered_bytes_ != buffer_.size())
      return;
    transform(std::span<const unsigned char, 64>(buffer_));
    buffered_bytes_ = 0;
  }

  while (input.size() >= buffer_.size()) {
    transform(std::span<const unsigned char, 64>(
        input.data(), buffer_.size()));
    input = input.subspan(buffer_.size());
  }
  std::copy(input.begin(), input.end(), buffer_.begin());
  buffered_bytes_ = input.size();
}

void Sha256::update(std::string_view input) {
  update(std::span<const unsigned char>(
      reinterpret_cast<const unsigned char *>(input.data()), input.size()));
}

Sha256::Digest Sha256::finalize() {
  if (finalized_)
    return digest_;

  std::array<unsigned char, 128> final_blocks{};
  std::copy_n(buffer_.begin(), buffered_bytes_, final_blocks.begin());
  final_blocks[buffered_bytes_] = 0x80u;
  const std::size_t final_bytes = buffered_bytes_ < 56u ? 64u : 128u;
  const std::uint64_t bit_length = total_bytes_ * 8u;
  for (unsigned int index = 0; index < 8u; ++index) {
    final_blocks[final_bytes - 1u - index] =
        static_cast<unsigned char>(bit_length >> (index * 8u));
  }
  transform(std::span<const unsigned char, 64>(final_blocks.data(), 64u));
  if (final_bytes == 128u) {
    transform(std::span<const unsigned char, 64>(
        final_blocks.data() + 64u, 64u));
  }

  std::size_t output = 0;
  for (const std::uint32_t word : state_) {
    for (int shift = 24; shift >= 0; shift -= 8) {
      digest_[output++] = static_cast<unsigned char>(word >> shift);
    }
  }
  finalized_ = true;
  return digest_;
}

std::string Sha256::finalizeHex() {
  const Digest bytes = finalize();
  static constexpr char kHex[] = "0123456789abcdef";
  std::string output(bytes.size() * 2u, '0');
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    output[index * 2u] = kHex[bytes[index] >> 4u];
    output[index * 2u + 1u] = kHex[bytes[index] & 0x0fu];
  }
  return output;
}

std::string sha256Hex(std::span<const unsigned char> input) {
  Sha256 digest;
  digest.update(input);
  return digest.finalizeHex();
}

std::string sha256Hex(std::string_view input) {
  Sha256 digest;
  digest.update(input);
  return digest.finalizeHex();
}

} // namespace astra::utils
