#include "astra/utils/Sha256.hpp"

#include <gtest/gtest.h>

#include <array>
#include <span>
#include <stdexcept>
#include <string>

TEST(Sha256Test, MatchesPublishedEmptyAndAbcVectors) {
  EXPECT_EQ(
      astra::utils::sha256Hex(std::string_view{}),
      "e3b0c44298fc1c149afbf4c8996fb924"
      "27ae41e4649b934ca495991b7852b855");
  EXPECT_EQ(
      astra::utils::sha256Hex(std::string_view{"abc"}),
      "ba7816bf8f01cfea414140de5dae2223"
      "b00361a396177a9cb410ff61f20015ad");
}

TEST(Sha256Test, StreamingAcrossBlockBoundariesMatchesOneShotHash) {
  std::string input;
  for (int index = 0; index < 1000; ++index)
    input += "0123456789abcdef";

  astra::utils::Sha256 streaming;
  const auto bytes = std::span<const unsigned char>(
      reinterpret_cast<const unsigned char *>(input.data()), input.size());
  streaming.update(bytes.first(1));
  streaming.update(bytes.subspan(1, 62));
  streaming.update(bytes.subspan(63, 65));
  streaming.update(bytes.subspan(128));

  EXPECT_EQ(streaming.finalizeHex(), astra::utils::sha256Hex(input));
  EXPECT_EQ(streaming.finalizeHex(), astra::utils::sha256Hex(input));
  EXPECT_THROW(streaming.update(std::string_view{"late"}), std::logic_error);
}

TEST(Sha256Test, MatchesMillionAStandardVector) {
  const std::string input(1'000'000, 'a');
  EXPECT_EQ(
      astra::utils::sha256Hex(input),
      "cdc76e5c9914fb9281a1c7e284d73e67"
      "f1809a48a497200e046d39ccc7112cd0");
}

TEST(Sha256Test, MatchesPaddingBoundaryVectors) {
  const auto input = [](std::size_t length) {
    std::string value(length, '\0');
    for (std::size_t index = 0; index < length; ++index)
      value[index] = static_cast<char>((index * 37u + 11u) & 0xffu);
    return value;
  };

  EXPECT_EQ(astra::utils::sha256Hex(input(55)),
            "2900465fcb533e05a158fd2b3be0e5e3"
            "b03740d83060aa3580e0d98a96bf2384");
  EXPECT_EQ(astra::utils::sha256Hex(input(56)),
            "31454ff48ef36af2f08fd511bdc37d9d5"
            "855ac23e992e5ff5445cb6b7674a674");
  EXPECT_EQ(astra::utils::sha256Hex(input(63)),
            "5f6401b96532c36de4e65beec0409b69"
            "b1d181864c8009b7a04f43e5d56350d1");
  EXPECT_EQ(astra::utils::sha256Hex(input(64)),
            "94eb5de4943613fd048dc93393ab06877"
            "405faa39c11f53e9386083339833e7e");
}
