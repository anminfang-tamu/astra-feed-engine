#include "astra/utils/Bitmap.hpp"

#include <bit>
#include <cstddef>

void Bitmap::set(uint32_t from_index) {
  const size_t word_index = from_index / kBitmapWordBits;
  if (word_index >= words_.size()) {
    words_.resize(word_index + 1, 0);
  }

  words_[word_index] |= uint64_t{1} << (from_index % kBitmapWordBits);
}

void Bitmap::reset(uint32_t from_index) {
  const size_t word_index = from_index / kBitmapWordBits;
  if (word_index >= words_.size()) {
    return;
  }

  words_[word_index] &= ~(uint64_t{1} << (from_index % kBitmapWordBits));
}

uint32_t Bitmap::findPrevSet(uint32_t index) const {
  if (words_.empty()) {
    return kNoIndex;
  }

  size_t word_index = index / kBitmapWordBits;
  uint64_t bits = 0;
  if (word_index >= words_.size()) {
    word_index = words_.size() - 1;
    bits = words_[word_index];
  } else {
    const uint32_t bit_index = index % kBitmapWordBits;
    bits = words_[word_index] & Bitmap::maskThroughBit(bit_index);
  }

  while (true) {
    if (bits != 0) {
      return static_cast<uint32_t>(word_index * kBitmapWordBits +
                                   (63 - std::countl_zero(bits)));
    }

    if (word_index == 0) {
      return kNoIndex;
    }

    --word_index;
    bits = words_[word_index];
  }
}

uint32_t Bitmap::findNextSet(uint32_t index) const {
  if (words_.empty()) {
    return kNoIndex;
  }

  size_t word_index = index / kBitmapWordBits;
  if (word_index >= words_.size()) {
    return kNoIndex;
  }

  const uint32_t bit_index = index % kBitmapWordBits;
  uint64_t bits = words_[word_index] & (~uint64_t{0} << bit_index);

  while (word_index < words_.size()) {
    if (bits != 0) {
      return static_cast<uint32_t>(word_index * kBitmapWordBits +
                                   std::countr_zero(bits));
    }

    ++word_index;
    if (word_index < words_.size()) {
      bits = words_[word_index];
    }
  }

  return kNoIndex;
}

uint64_t Bitmap::maskThroughBit(uint32_t bit_index) const {
  return bit_index == 63 ? ~uint64_t{0}
                         : ((uint64_t{1} << (bit_index + 1)) - 1);
}
