#include "astra/book/BookManager.hpp"
#include "astra/utils/Sha256.hpp"

#include <charconv>
#include <cstdint>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::size_t kMaximumManifestBytes = 16u * 1024u;
constexpr std::size_t kManifestV1LineCount = 11;
constexpr std::size_t kManifestV2LineCount = 12;

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

std::vector<std::string_view>
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

  std::vector<std::string_view> lines;
  lines.reserve(kManifestV2LineCount);
  std::size_t begin = 0;
  while (begin < contents.size()) {
    const std::size_t end = contents.find('\n', begin);
    if (end == std::string::npos || lines.size() == kManifestV2LineCount)
      throw std::invalid_argument(
          "capacity evidence manifest has an unexpected line count");
    lines.push_back(
        std::string_view(contents).substr(begin, end - begin));
    begin = end + 1u;
  }
  if (lines.size() != kManifestV1LineCount &&
      lines.size() != kManifestV2LineCount) {
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
  const std::string computed_sha256 = astra::utils::sha256Hex(contents);
  if (computed_sha256 != expected_sha256) {
    throw std::invalid_argument(
        "capacity evidence manifest SHA-256 differs from configured digest");
  }

  const auto lines = canonicalLines(contents);
  const std::string_view schema = field(lines[0], "schema");
  const bool is_v1 = schema == BookCapacityEvidenceManifest::kSchemaV1;
  const bool is_v2 = schema == BookCapacityEvidenceManifest::kSchemaV2;
  if ((!is_v1 && !is_v2) ||
      (is_v1 && lines.size() != kManifestV1LineCount) ||
      (is_v2 && lines.size() != kManifestV2LineCount)) {
    throw std::invalid_argument("unsupported capacity evidence manifest schema");
  }
  const std::string_view profile_name = field(lines[1], "profile_name");
  if (!isProfileToken(profile_name) || profile_name != expected_profile_name) {
    throw std::invalid_argument(
        "capacity evidence manifest profile_name differs from configured profile");
  }
  const std::string_view corpus_sha256 =
      field(lines[2], "corpus_manifest_sha256");
  const std::string_view profiler_sha256 =
      field(lines[3], "profiler_sha256");
  const std::string_view profile_output_sha256 =
      is_v2 ? field(lines[4], "profile_output_sha256") : std::string_view{};
  if (!isLowerHexSha256(corpus_sha256) ||
      !isLowerHexSha256(profiler_sha256) ||
      (is_v2 && !isLowerHexSha256(profile_output_sha256))) {
    throw std::invalid_argument(
        "capacity evidence manifest provenance hashes must be lowercase SHA-256");
  }

  const std::size_t capacity_offset = is_v2 ? 1u : 0u;
  const std::uint64_t direct_slots = canonicalUnsigned(
      field(lines[4u + capacity_offset], "order_direct_slots"),
      "order_direct_slots",
      std::numeric_limits<std::uint64_t>::max());
  const std::uint64_t fallback_buckets = canonicalUnsigned(
      field(lines[5u + capacity_offset], "order_fallback_buckets"),
      "order_fallback_buckets",
      std::numeric_limits<std::uint32_t>::max());
  const std::uint64_t price_pages = canonicalUnsigned(
      field(lines[6u + capacity_offset], "price_page_capacity"),
      "price_page_capacity",
      static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) -
          1u);
  const std::uint64_t maximum_order_reference = canonicalUnsigned(
      field(lines[7u + capacity_offset], "profiled_max_order_ref"),
      "profiled_max_order_ref",
      std::numeric_limits<std::uint64_t>::max());
  const std::uint64_t unique_price_pages = canonicalUnsigned(
      field(lines[8u + capacity_offset], "profiled_unique_price_pages"),
      "profiled_unique_price_pages",
      static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) -
          1u);
  const std::uint64_t minimum_direct_headroom = canonicalUnsigned(
      field(lines[9u + capacity_offset], "minimum_direct_order_headroom"),
      "minimum_direct_order_headroom",
      std::numeric_limits<std::uint64_t>::max());
  const std::uint64_t minimum_price_headroom = canonicalUnsigned(
      field(lines[10u + capacity_offset], "minimum_price_page_headroom"),
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
          std::string(profiler_sha256),
          std::string(profile_output_sha256), std::string(schema)};
}
