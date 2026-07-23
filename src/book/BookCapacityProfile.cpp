#include "astra/book/BookManager.hpp"

#include <array>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::size_t kMaximumManifestBytes = 16u * 1024u;
constexpr std::size_t kManifestLineCount = 11;

bool isLowerHexSha256(std::string_view value) noexcept {
  if (value.size() != 64)
    return false;
  for (const char character : value) {
    if (!((character >= '0' && character <= '9') ||
          (character >= 'a' && character <= 'f'))) {
      return false;
    }
  }
  return true;
}

bool isProfileToken(std::string_view value) noexcept {
  if (value.empty() || value.size() > 128)
    return false;
  for (const unsigned char character : value) {
    const bool ascii_alphanumeric =
        (character >= '0' && character <= '9') ||
        (character >= 'A' && character <= 'Z') ||
        (character >= 'a' && character <= 'z');
    if (!ascii_alphanumeric && character != '-' && character != '_' &&
        character != '.' && character != ':' && character != '/' &&
        character != '+') {
      return false;
    }
  }
  return true;
}

constexpr std::uint32_t rotateRight(std::uint32_t value,
                                    unsigned int bits) noexcept {
  return (value >> bits) | (value << (32u - bits));
}

std::string sha256(std::span<const unsigned char> input) {
  static constexpr std::array<std::uint32_t, 64> round_constants{
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

  std::vector<unsigned char> padded(input.begin(), input.end());
  const std::uint64_t bit_length =
      static_cast<std::uint64_t>(input.size()) * 8u;
  padded.push_back(0x80u);
  while (padded.size() % 64u != 56u)
    padded.push_back(0u);
  for (int shift = 56; shift >= 0; shift -= 8)
    padded.push_back(static_cast<unsigned char>(bit_length >> shift));

  std::array<std::uint32_t, 8> state{
      0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
      0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
  std::array<std::uint32_t, 64> words{};
  for (std::size_t block = 0; block < padded.size(); block += 64u) {
    for (std::size_t index = 0; index < 16u; ++index) {
      const std::size_t offset = block + index * 4u;
      words[index] =
          (static_cast<std::uint32_t>(padded[offset]) << 24u) |
          (static_cast<std::uint32_t>(padded[offset + 1u]) << 16u) |
          (static_cast<std::uint32_t>(padded[offset + 2u]) << 8u) |
          static_cast<std::uint32_t>(padded[offset + 3u]);
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

    std::uint32_t a = state[0];
    std::uint32_t b = state[1];
    std::uint32_t c = state[2];
    std::uint32_t d = state[3];
    std::uint32_t e = state[4];
    std::uint32_t f = state[5];
    std::uint32_t g = state[6];
    std::uint32_t h = state[7];
    for (std::size_t index = 0; index < words.size(); ++index) {
      const std::uint32_t sum1 =
          rotateRight(e, 6u) ^ rotateRight(e, 11u) ^ rotateRight(e, 25u);
      const std::uint32_t choose = (e & f) ^ (~e & g);
      const std::uint32_t temporary1 =
          h + sum1 + choose + round_constants[index] + words[index];
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
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
  }

  static constexpr char hex[] = "0123456789abcdef";
  std::string digest(64, '0');
  std::size_t output = 0;
  for (const std::uint32_t word : state) {
    for (int shift = 28; shift >= 0; shift -= 4)
      digest[output++] = hex[(word >> shift) & 0x0fu];
  }
  return digest;
}

std::string readManifest(std::string_view path) {
  if (path.empty())
    throw std::invalid_argument("capacity evidence manifest path is empty");
  std::ifstream input(std::string(path), std::ios::binary | std::ios::ate);
  if (!input.is_open())
    throw std::invalid_argument("cannot open capacity evidence manifest");
  const std::streampos end = input.tellg();
  if (end <= 0 ||
      static_cast<std::uint64_t>(end) > kMaximumManifestBytes) {
    throw std::invalid_argument(
        "capacity evidence manifest size must be in [1, 16384] bytes");
  }
  std::string contents(static_cast<std::size_t>(end), '\0');
  input.seekg(0);
  input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
  if (!input || input.gcount() != static_cast<std::streamsize>(contents.size()))
    throw std::invalid_argument("cannot read complete capacity evidence manifest");
  return contents;
}

std::array<std::string_view, kManifestLineCount>
canonicalLines(const std::string &contents) {
  if (contents.back() != '\n')
    throw std::invalid_argument(
        "capacity evidence manifest must end with one LF");
  for (const unsigned char character : contents) {
    if (character == '\r' || character == '\0' || character > 0x7fu) {
      throw std::invalid_argument(
          "capacity evidence manifest must be canonical ASCII with LF endings");
    }
  }

  std::array<std::string_view, kManifestLineCount> lines{};
  std::size_t count = 0;
  std::size_t begin = 0;
  while (begin < contents.size()) {
    const std::size_t end = contents.find('\n', begin);
    if (end == std::string::npos || count == lines.size())
      throw std::invalid_argument(
          "capacity evidence manifest has an unexpected line count");
    lines[count++] = std::string_view(contents).substr(begin, end - begin);
    begin = end + 1u;
  }
  if (count != lines.size()) {
    throw std::invalid_argument(
        "capacity evidence manifest has an unexpected line count");
  }
  return lines;
}

std::string_view field(std::string_view line, std::string_view key) {
  if (!line.starts_with(key) || line.size() == key.size() ||
      line[key.size()] != '=') {
    throw std::invalid_argument(
        "capacity evidence manifest keys/order do not match schema");
  }
  return line.substr(key.size() + 1u);
}

std::uint64_t canonicalUnsigned(std::string_view value, std::string_view key,
                                std::uint64_t maximum) {
  if (value.empty() || (value.size() > 1 && value.front() == '0')) {
    throw std::invalid_argument("capacity evidence manifest " +
                                std::string(key) +
                                " is not a canonical positive integer");
  }
  std::uint64_t parsed = 0;
  const auto result =
      std::from_chars(value.data(), value.data() + value.size(), parsed, 10);
  if (result.ec != std::errc{} ||
      result.ptr != value.data() + value.size() || parsed == 0 ||
      parsed > maximum) {
    throw std::invalid_argument("capacity evidence manifest " +
                                std::string(key) +
                                " is not an in-range positive integer");
  }
  return parsed;
}

} // namespace

BookCapacityEvidenceManifest loadBookCapacityEvidenceManifest(
    std::string_view path, std::string_view expected_profile_name,
    std::string_view expected_sha256) {
  if (!isProfileToken(expected_profile_name)) {
    throw std::invalid_argument(
        "expected capacity profile name is not an audit-safe token");
  }
  if (!isLowerHexSha256(expected_sha256)) {
    throw std::invalid_argument(
        "expected capacity evidence SHA-256 must be 64 lowercase hex digits");
  }

  const std::string contents = readManifest(path);
  const std::string computed_sha256 = sha256(std::span<const unsigned char>(
      reinterpret_cast<const unsigned char *>(contents.data()),
      contents.size()));
  if (computed_sha256 != expected_sha256) {
    throw std::invalid_argument(
        "capacity evidence manifest SHA-256 differs from configured digest");
  }

  const auto lines = canonicalLines(contents);
  const std::string_view schema = field(lines[0], "schema");
  if (schema != BookCapacityEvidenceManifest::kSchema)
    throw std::invalid_argument("unsupported capacity evidence manifest schema");
  const std::string_view profile_name = field(lines[1], "profile_name");
  if (!isProfileToken(profile_name) || profile_name != expected_profile_name) {
    throw std::invalid_argument(
        "capacity evidence manifest profile_name differs from configured profile");
  }
  const std::string_view corpus_sha256 =
      field(lines[2], "corpus_manifest_sha256");
  const std::string_view profiler_sha256 =
      field(lines[3], "profiler_sha256");
  if (!isLowerHexSha256(corpus_sha256) ||
      !isLowerHexSha256(profiler_sha256)) {
    throw std::invalid_argument(
        "capacity evidence manifest provenance hashes must be lowercase SHA-256");
  }

  const std::uint64_t direct_slots = canonicalUnsigned(
      field(lines[4], "order_direct_slots"), "order_direct_slots",
      std::numeric_limits<std::uint64_t>::max());
  const std::uint64_t fallback_buckets = canonicalUnsigned(
      field(lines[5], "order_fallback_buckets"), "order_fallback_buckets",
      std::numeric_limits<std::uint32_t>::max());
  const std::uint64_t price_pages = canonicalUnsigned(
      field(lines[6], "price_page_capacity"), "price_page_capacity",
      static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) -
          1u);
  const std::uint64_t maximum_order_reference = canonicalUnsigned(
      field(lines[7], "profiled_max_order_ref"), "profiled_max_order_ref",
      std::numeric_limits<std::uint64_t>::max());
  const std::uint64_t unique_price_pages = canonicalUnsigned(
      field(lines[8], "profiled_unique_price_pages"),
      "profiled_unique_price_pages",
      static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) -
          1u);
  const std::uint64_t minimum_direct_headroom = canonicalUnsigned(
      field(lines[9], "minimum_direct_order_headroom"),
      "minimum_direct_order_headroom",
      std::numeric_limits<std::uint64_t>::max());
  const std::uint64_t minimum_price_headroom = canonicalUnsigned(
      field(lines[10], "minimum_price_page_headroom"),
      "minimum_price_page_headroom",
      std::numeric_limits<std::uint32_t>::max());

  BookCapacityProfile profile{
      std::string(profile_name),
      computed_sha256,
      {{direct_slots, static_cast<std::uint32_t>(fallback_buckets), false},
       {static_cast<std::uint32_t>(price_pages), false}},
      {maximum_order_reference,
       static_cast<std::uint32_t>(unique_price_pages)},
      {minimum_direct_headroom,
       static_cast<std::uint32_t>(minimum_price_headroom)}};
  validateBookCapacityProfile(profile);
  return {std::move(profile), std::string(corpus_sha256),
          std::string(profiler_sha256)};
}
