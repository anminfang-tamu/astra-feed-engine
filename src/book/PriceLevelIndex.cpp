#include "astra/book/PriceLevelIndex.hpp"

#include <algorithm>
#include <bit>
#include <stdexcept>

namespace {

size_t checkedHandleCapacity(size_t capacity) {
  if (capacity > std::numeric_limits<uint32_t>::max()) {
    throw std::length_error("price-level pool exceeds 32-bit handle domain");
  }
  return capacity;
}

uint8_t priceByte(uint32_t price, unsigned shift) noexcept {
  return static_cast<uint8_t>(price >> shift);
}

uint32_t assemblePrice(uint8_t byte3, uint8_t byte2, uint8_t byte1,
                       uint8_t byte0) noexcept {
  return (static_cast<uint32_t>(byte3) << 24) |
         (static_cast<uint32_t>(byte2) << 16) |
         (static_cast<uint32_t>(byte1) << 8) |
         static_cast<uint32_t>(byte0);
}

template <typename T> void touchPages(std::vector<T> &storage) noexcept {
  if (storage.empty()) {
    return;
  }
  constexpr size_t kPageSize = 4096;
  const size_t byte_count = storage.size() * sizeof(T);
  volatile std::byte *bytes =
      reinterpret_cast<volatile std::byte *>(storage.data());
  for (size_t offset = 0; offset < byte_count; offset += kPageSize) {
    bytes[offset] = bytes[offset];
  }
  bytes[byte_count - 1] = bytes[byte_count - 1];
}

} // namespace

void PriceLevelArena::RadixNode::reset() noexcept {
  for (auto &side : occupied) {
    side.fill(0);
  }
  children.fill(NodeHandle{});
}

void PriceLevelArena::PriceLeaf::reset() noexcept {
  for (auto &side : occupied) {
    side.fill(0);
  }
  for (auto &side : levels) {
    side.fill(PriceLevelHandle{});
  }
}

PriceLevelArena::PriceLevelArena(PriceLevelArenaConfig config)
    : nodes_(checkedHandleCapacity(config.internal_node_capacity)),
      leaves_(checkedHandleCapacity(config.leaf_capacity)),
      levels_(checkedHandleCapacity(config.level_capacity)),
      free_nodes_(nodes_.size()), free_leaves_(leaves_.size()),
      free_levels_(levels_.size()) {
  // Sized vectors are value-initialized exactly once. The page walk makes
  // residency explicit without clearing every large pool a second time. The
  // free arrays are sized rather than merely reserved, so releases never
  // allocate.
  touchPages(nodes_);
  touchPages(leaves_);
  touchPages(levels_);
  touchPages(free_nodes_);
  touchPages(free_leaves_);
  touchPages(free_levels_);
}

PooledPriceLevel *PriceLevelArena::level(PriceLevelHandle handle) noexcept {
  if (!handle.valid() || handle.value > levels_.size()) {
    return nullptr;
  }
  return &levels_[handle.value - 1];
}

const PooledPriceLevel *
PriceLevelArena::level(PriceLevelHandle handle) const noexcept {
  if (!handle.valid() || handle.value > levels_.size()) {
    return nullptr;
  }
  return &levels_[handle.value - 1];
}

size_t PriceLevelArena::availableInternalNodes() const noexcept {
  return nodes_.size() - (next_node_ - free_node_count_);
}

size_t PriceLevelArena::availableLeaves() const noexcept {
  return leaves_.size() - (next_leaf_ - free_leaf_count_);
}

size_t PriceLevelArena::availableLevels() const noexcept {
  return levels_.size() - (next_level_ - free_level_count_);
}

PriceLevelArenaStats PriceLevelArena::stats() const noexcept {
  PriceLevelArenaStats result;
  result.internal_node_capacity = nodes_.size();
  result.internal_nodes_in_use = next_node_ - free_node_count_;
  result.internal_node_high_watermark = node_high_watermark_;
  result.leaf_capacity = leaves_.size();
  result.leaves_in_use = next_leaf_ - free_leaf_count_;
  result.leaf_high_watermark = leaf_high_watermark_;
  result.level_capacity = levels_.size();
  result.levels_in_use = next_level_ - free_level_count_;
  result.level_high_watermark = level_high_watermark_;
  result.internal_node_exhaustions = internal_node_exhaustions_;
  result.leaf_exhaustions = leaf_exhaustions_;
  result.level_exhaustions = level_exhaustions_;
  return result;
}

PriceLevelArena::NodeHandle PriceLevelArena::allocateNode() noexcept {
  uint32_t index = 0;
  if (free_node_count_ != 0) {
    index = free_nodes_[--free_node_count_];
  } else {
    if (next_node_ >= nodes_.size()) {
      return {};
    }
    index = static_cast<uint32_t>(next_node_++);
  }
  node_high_watermark_ =
      std::max(node_high_watermark_, next_node_ - free_node_count_);
  return NodeHandle{index + 1};
}

PriceLevelArena::LeafHandle PriceLevelArena::allocateLeaf() noexcept {
  uint32_t index = 0;
  if (free_leaf_count_ != 0) {
    index = free_leaves_[--free_leaf_count_];
  } else {
    if (next_leaf_ >= leaves_.size()) {
      return {};
    }
    index = static_cast<uint32_t>(next_leaf_++);
  }
  leaf_high_watermark_ =
      std::max(leaf_high_watermark_, next_leaf_ - free_leaf_count_);
  return LeafHandle{index + 1};
}

PriceLevelHandle PriceLevelArena::allocateLevel() noexcept {
  uint32_t index = 0;
  if (free_level_count_ != 0) {
    index = free_levels_[--free_level_count_];
  } else {
    if (next_level_ >= levels_.size()) {
      return {};
    }
    index = static_cast<uint32_t>(next_level_++);
  }
  level_high_watermark_ =
      std::max(level_high_watermark_, next_level_ - free_level_count_);
  return PriceLevelHandle{index + 1};
}

void PriceLevelArena::releaseNode(NodeHandle handle) noexcept {
  if (!handle.valid() || handle.value > next_node_ ||
      free_node_count_ >= free_nodes_.size()) {
    return;
  }
  const uint32_t index = handle.value - 1;
  nodes_[index].reset();
  free_nodes_[free_node_count_++] = index;
}

void PriceLevelArena::releaseLeaf(LeafHandle handle) noexcept {
  if (!handle.valid() || handle.value > next_leaf_ ||
      free_leaf_count_ >= free_leaves_.size()) {
    return;
  }
  const uint32_t index = handle.value - 1;
  leaves_[index].reset();
  free_leaves_[free_leaf_count_++] = index;
}

void PriceLevelArena::releaseLevel(PriceLevelHandle handle) noexcept {
  if (!handle.valid() || handle.value > next_level_ ||
      free_level_count_ >= free_levels_.size()) {
    return;
  }
  const uint32_t index = handle.value - 1;
  levels_[index] = PooledPriceLevel{};
  free_levels_[free_level_count_++] = index;
}

PriceLevelArena::RadixNode *
PriceLevelArena::node(NodeHandle handle) noexcept {
  if (!handle.valid() || handle.value > nodes_.size()) {
    return nullptr;
  }
  return &nodes_[handle.value - 1];
}

const PriceLevelArena::RadixNode *
PriceLevelArena::node(NodeHandle handle) const noexcept {
  if (!handle.valid() || handle.value > nodes_.size()) {
    return nullptr;
  }
  return &nodes_[handle.value - 1];
}

PriceLevelArena::PriceLeaf *
PriceLevelArena::leaf(LeafHandle handle) noexcept {
  if (!handle.valid() || handle.value > leaves_.size()) {
    return nullptr;
  }
  return &leaves_[handle.value - 1];
}

const PriceLevelArena::PriceLeaf *
PriceLevelArena::leaf(LeafHandle handle) const noexcept {
  if (!handle.valid() || handle.value > leaves_.size()) {
    return nullptr;
  }
  return &leaves_[handle.value - 1];
}

PriceLevelIndex::PriceLevelIndex(PriceLevelArena &arena) noexcept
    : arena_(&arena) {
  root_.reset();
}

PriceLevelIndex::~PriceLevelIndex() { clear(); }

bool PriceLevelIndex::bitmapEmpty(
    const std::array<uint64_t, 4> &bits) noexcept {
  return (bits[0] | bits[1] | bits[2] | bits[3]) == 0;
}

bool PriceLevelIndex::bitmapTest(const std::array<uint64_t, 4> &bits,
                                 uint8_t index) noexcept {
  return (bits[index >> 6] & (uint64_t{1} << (index & 63))) != 0;
}

void PriceLevelIndex::bitmapSet(std::array<uint64_t, 4> &bits,
                                uint8_t index) noexcept {
  bits[index >> 6] |= uint64_t{1} << (index & 63);
}

void PriceLevelIndex::bitmapClear(std::array<uint64_t, 4> &bits,
                                  uint8_t index) noexcept {
  bits[index >> 6] &= ~(uint64_t{1} << (index & 63));
}

int PriceLevelIndex::bitmapFirst(
    const std::array<uint64_t, 4> &bits) noexcept {
  for (uint32_t word = 0; word < bits.size(); ++word) {
    if (bits[word] != 0) {
      return static_cast<int>((word << 6) + std::countr_zero(bits[word]));
    }
  }
  return -1;
}

int PriceLevelIndex::bitmapLast(
    const std::array<uint64_t, 4> &bits) noexcept {
  for (int word = static_cast<int>(bits.size()) - 1; word >= 0; --word) {
    if (bits[static_cast<size_t>(word)] != 0) {
      return (word << 6) + 63 -
             static_cast<int>(
                 std::countl_zero(bits[static_cast<size_t>(word)]));
    }
  }
  return -1;
}

int PriceLevelIndex::bitmapNext(const std::array<uint64_t, 4> &bits,
                                uint8_t index) noexcept {
  if (index == std::numeric_limits<uint8_t>::max()) {
    return -1;
  }
  const uint32_t start = static_cast<uint32_t>(index) + 1;
  uint32_t word = start >> 6;
  uint64_t candidates = bits[word] & (~uint64_t{0} << (start & 63));
  if (candidates != 0) {
    return static_cast<int>((word << 6) + std::countr_zero(candidates));
  }
  for (++word; word < bits.size(); ++word) {
    if (bits[word] != 0) {
      return static_cast<int>((word << 6) + std::countr_zero(bits[word]));
    }
  }
  return -1;
}

int PriceLevelIndex::bitmapPrevious(const std::array<uint64_t, 4> &bits,
                                    uint8_t index) noexcept {
  if (index == 0) {
    return -1;
  }
  const uint32_t start = static_cast<uint32_t>(index) - 1;
  int word = static_cast<int>(start >> 6);
  const uint32_t bit = start & 63;
  const uint64_t mask = bit == 63 ? ~uint64_t{0}
                                  : ((uint64_t{1} << (bit + 1)) - 1);
  uint64_t candidates = bits[static_cast<size_t>(word)] & mask;
  if (candidates != 0) {
    return (word << 6) + 63 -
           static_cast<int>(std::countl_zero(candidates));
  }
  for (--word; word >= 0; --word) {
    candidates = bits[static_cast<size_t>(word)];
    if (candidates != 0) {
      return (word << 6) + 63 -
             static_cast<int>(std::countl_zero(candidates));
    }
  }
  return -1;
}

PriceLevelIndex::EnsureResult
PriceLevelIndex::ensure(uint32_t price, Side side) noexcept {
  const size_t side_index = sideIndex(side);
  const uint8_t byte3 = priceByte(price, 24);
  const uint8_t byte2 = priceByte(price, 16);
  const uint8_t byte1 = priceByte(price, 8);
  const uint8_t byte0 = priceByte(price, 0);

  NodeHandle first = root_.children[byte3];
  NodeHandle second{};
  LeafHandle leaf_handle{};

  if (first.valid()) {
    const RadixNode *first_node = arena_->node(first);
    if (first_node != nullptr) {
      second = first_node->children[byte2];
    }
  }
  if (second.valid()) {
    const RadixNode *second_node = arena_->node(second);
    if (second_node != nullptr) {
      leaf_handle = LeafHandle{second_node->children[byte1].value};
    }
  }
  if (leaf_handle.valid()) {
    const PriceLeaf *leaf = arena_->leaf(leaf_handle);
    if (leaf != nullptr) {
      const PriceLevelHandle existing = leaf->levels[side_index][byte0];
      if (existing.valid()) {
        return {EnsureStatus::Found, existing};
      }
    }
  }

  const size_t needed_nodes = !first.valid() ? 2 : (!second.valid() ? 1 : 0);
  const size_t needed_leaves = leaf_handle.valid() ? 0 : 1;
  if (arena_->availableInternalNodes() < needed_nodes) {
    arena_->recordInternalNodeExhaustion();
    return {EnsureStatus::InternalNodePoolExhausted, {}};
  }
  if (arena_->availableLeaves() < needed_leaves) {
    arena_->recordLeafExhaustion();
    return {EnsureStatus::LeafPoolExhausted, {}};
  }
  if (arena_->availableLevels() == 0) {
    arena_->recordLevelExhaustion();
    return {EnsureStatus::LevelPoolExhausted, {}};
  }

  NodeHandle new_first{};
  NodeHandle new_second{};
  LeafHandle new_leaf{};
  if (!first.valid()) {
    new_first = arena_->allocateNode();
    new_second = arena_->allocateNode();
    first = new_first;
    second = new_second;
  } else if (!second.valid()) {
    new_second = arena_->allocateNode();
    second = new_second;
  }
  if (!leaf_handle.valid()) {
    new_leaf = arena_->allocateLeaf();
    leaf_handle = new_leaf;
  }
  const PriceLevelHandle level_handle = arena_->allocateLevel();

  // Preflight above makes these failures unreachable in the single-writer
  // model. Keep the rollback defensive so a future allocator change cannot
  // publish a partial path.
  if (!first.valid() || !second.valid() || !leaf_handle.valid() ||
      !level_handle.valid()) {
    if (level_handle.valid())
      arena_->releaseLevel(level_handle);
    if (new_leaf.valid())
      arena_->releaseLeaf(new_leaf);
    if (new_second.valid())
      arena_->releaseNode(new_second);
    if (new_first.valid())
      arena_->releaseNode(new_first);
    arena_->recordLevelExhaustion();
    return {EnsureStatus::LevelPoolExhausted, {}};
  }

  if (new_first.valid()) {
    root_.children[byte3] = first;
  }
  RadixNode *first_node = arena_->node(first);
  if (new_second.valid()) {
    first_node->children[byte2] = second;
  }
  RadixNode *second_node = arena_->node(second);
  if (new_leaf.valid()) {
    second_node->children[byte1] = NodeHandle{leaf_handle.value};
  }
  PriceLeaf *leaf = arena_->leaf(leaf_handle);
  leaf->levels[side_index][byte0] = level_handle;
  bitmapSet(leaf->occupied[side_index], byte0);
  bitmapSet(second_node->occupied[side_index], byte1);
  bitmapSet(first_node->occupied[side_index], byte2);
  bitmapSet(root_.occupied[side_index], byte3);
  return {EnsureStatus::Created, level_handle};
}

PriceLevelHandle PriceLevelIndex::find(uint32_t price, Side side) const
    noexcept {
  const size_t side_index = sideIndex(side);
  const uint8_t byte3 = priceByte(price, 24);
  const uint8_t byte2 = priceByte(price, 16);
  const uint8_t byte1 = priceByte(price, 8);
  const uint8_t byte0 = priceByte(price, 0);
  if (!bitmapTest(root_.occupied[side_index], byte3)) {
    return {};
  }
  const NodeHandle first = root_.children[byte3];
  const RadixNode *first_node = arena_->node(first);
  if (first_node == nullptr ||
      !bitmapTest(first_node->occupied[side_index], byte2)) {
    return {};
  }
  const NodeHandle second = first_node->children[byte2];
  const RadixNode *second_node = arena_->node(second);
  if (second_node == nullptr ||
      !bitmapTest(second_node->occupied[side_index], byte1)) {
    return {};
  }
  const LeafHandle leaf_handle{second_node->children[byte1].value};
  const PriceLeaf *leaf = arena_->leaf(leaf_handle);
  if (leaf == nullptr || !bitmapTest(leaf->occupied[side_index], byte0)) {
    return {};
  }
  return leaf->levels[side_index][byte0];
}

PriceLevelIndex::PricePoint
PriceLevelIndex::pointFromLeaf(LeafHandle leaf_handle, uint8_t byte3,
                               uint8_t byte2, uint8_t byte1, Side side,
                               bool high) const noexcept {
  const PriceLeaf *leaf = arena_->leaf(leaf_handle);
  if (leaf == nullptr) {
    return {};
  }
  const size_t side_index = sideIndex(side);
  const int selected = high ? bitmapLast(leaf->occupied[side_index])
                            : bitmapFirst(leaf->occupied[side_index]);
  if (selected < 0) {
    return {};
  }
  const uint8_t byte0 = static_cast<uint8_t>(selected);
  return {assemblePrice(byte3, byte2, byte1, byte0),
          leaf->levels[side_index][byte0]};
}

PriceLevelIndex::PricePoint PriceLevelIndex::descendFromSecond(
    NodeHandle second, uint8_t byte3, uint8_t byte2, Side side,
    bool high) const noexcept {
  const RadixNode *second_node = arena_->node(second);
  if (second_node == nullptr) {
    return {};
  }
  const auto &bits = second_node->occupied[sideIndex(side)];
  const int selected = high ? bitmapLast(bits) : bitmapFirst(bits);
  if (selected < 0) {
    return {};
  }
  const uint8_t byte1 = static_cast<uint8_t>(selected);
  return pointFromLeaf(LeafHandle{second_node->children[byte1].value}, byte3,
                       byte2, byte1, side, high);
}

PriceLevelIndex::PricePoint
PriceLevelIndex::descendBest(NodeHandle first, uint8_t byte3, Side side,
                             bool high) const noexcept {
  const RadixNode *first_node = arena_->node(first);
  if (first_node == nullptr) {
    return {};
  }
  const auto &bits = first_node->occupied[sideIndex(side)];
  const int selected = high ? bitmapLast(bits) : bitmapFirst(bits);
  if (selected < 0) {
    return {};
  }
  const uint8_t byte2 = static_cast<uint8_t>(selected);
  return descendFromSecond(first_node->children[byte2], byte3, byte2, side,
                           high);
}

PriceLevelIndex::PricePoint PriceLevelIndex::best(Side side) const noexcept {
  const bool high = side == Side::Bid;
  const auto &bits = root_.occupied[sideIndex(side)];
  const int selected = high ? bitmapLast(bits) : bitmapFirst(bits);
  if (selected < 0) {
    return {};
  }
  const uint8_t byte3 = static_cast<uint8_t>(selected);
  return descendBest(root_.children[byte3], byte3, side, high);
}

PriceLevelIndex::PricePoint
PriceLevelIndex::nextWorse(Side side, uint32_t price) const noexcept {
  const bool high = side == Side::Bid;
  const size_t side_index = sideIndex(side);
  const uint8_t byte3 = priceByte(price, 24);
  const uint8_t byte2 = priceByte(price, 16);
  const uint8_t byte1 = priceByte(price, 8);
  const uint8_t byte0 = priceByte(price, 0);
  const auto next_index = [high](const std::array<uint64_t, 4> &bits,
                                 uint8_t index) noexcept {
    return high ? bitmapPrevious(bits, index) : bitmapNext(bits, index);
  };

  if (bitmapTest(root_.occupied[side_index], byte3)) {
    const NodeHandle first = root_.children[byte3];
    const RadixNode *first_node = arena_->node(first);
    if (first_node != nullptr &&
        bitmapTest(first_node->occupied[side_index], byte2)) {
      const NodeHandle second = first_node->children[byte2];
      const RadixNode *second_node = arena_->node(second);
      if (second_node != nullptr &&
          bitmapTest(second_node->occupied[side_index], byte1)) {
        const LeafHandle leaf_handle{
            second_node->children[byte1].value};
        const PriceLeaf *leaf = arena_->leaf(leaf_handle);
        if (leaf != nullptr) {
          const int next_byte0 =
              next_index(leaf->occupied[side_index], byte0);
          if (next_byte0 >= 0) {
            const auto selected = static_cast<uint8_t>(next_byte0);
            return {assemblePrice(byte3, byte2, byte1, selected),
                    leaf->levels[side_index][selected]};
          }
        }
      }
      if (second_node != nullptr) {
        const int next_byte1 =
            next_index(second_node->occupied[side_index], byte1);
        if (next_byte1 >= 0) {
          const auto selected = static_cast<uint8_t>(next_byte1);
          return pointFromLeaf(
              LeafHandle{second_node->children[selected].value}, byte3,
              byte2, selected, side, high);
        }
      }
    }
    if (first_node != nullptr) {
      const int next_byte2 =
          next_index(first_node->occupied[side_index], byte2);
      if (next_byte2 >= 0) {
        const auto selected = static_cast<uint8_t>(next_byte2);
        return descendFromSecond(first_node->children[selected], byte3,
                                 selected, side, high);
      }
    }
  }
  const int next_byte3 = next_index(root_.occupied[side_index], byte3);
  if (next_byte3 < 0) {
    return {};
  }
  const auto selected = static_cast<uint8_t>(next_byte3);
  return descendBest(root_.children[selected], selected, side, high);
}

PriceLevelIndex::EraseStatus
PriceLevelIndex::eraseEmpty(uint32_t price, Side side,
                            PriceLevelHandle expected) noexcept {
  const size_t side_index = sideIndex(side);
  const uint8_t byte3 = priceByte(price, 24);
  const uint8_t byte2 = priceByte(price, 16);
  const uint8_t byte1 = priceByte(price, 8);
  const uint8_t byte0 = priceByte(price, 0);

  if (!bitmapTest(root_.occupied[side_index], byte3)) {
    return EraseStatus::NotFound;
  }
  const NodeHandle first = root_.children[byte3];
  RadixNode *first_node = arena_->node(first);
  if (first_node == nullptr ||
      !bitmapTest(first_node->occupied[side_index], byte2)) {
    return EraseStatus::NotFound;
  }
  const NodeHandle second = first_node->children[byte2];
  RadixNode *second_node = arena_->node(second);
  if (second_node == nullptr ||
      !bitmapTest(second_node->occupied[side_index], byte1)) {
    return EraseStatus::NotFound;
  }
  const LeafHandle leaf_handle{second_node->children[byte1].value};
  PriceLeaf *leaf = arena_->leaf(leaf_handle);
  if (leaf == nullptr || !bitmapTest(leaf->occupied[side_index], byte0)) {
    return EraseStatus::NotFound;
  }
  const PriceLevelHandle level_handle = leaf->levels[side_index][byte0];
  if (expected.valid() && expected != level_handle) {
    return EraseStatus::HandleMismatch;
  }
  PooledPriceLevel *level_record = arena_->level(level_handle);
  if (level_record == nullptr) {
    return EraseStatus::NotFound;
  }
  if (!level_record->empty()) {
    return EraseStatus::LevelNotEmpty;
  }

  leaf->levels[side_index][byte0] = {};
  bitmapClear(leaf->occupied[side_index], byte0);
  arena_->releaseLevel(level_handle);

  if (bitmapEmpty(leaf->occupied[side_index])) {
    bitmapClear(second_node->occupied[side_index], byte1);
  }
  if (bitmapEmpty(second_node->occupied[side_index])) {
    bitmapClear(first_node->occupied[side_index], byte2);
  }
  if (bitmapEmpty(first_node->occupied[side_index])) {
    bitmapClear(root_.occupied[side_index], byte3);
  }

  if (bitmapEmpty(leaf->occupied[0]) && bitmapEmpty(leaf->occupied[1])) {
    second_node->children[byte1] = {};
    arena_->releaseLeaf(leaf_handle);
  }
  if (bitmapEmpty(second_node->occupied[0]) &&
      bitmapEmpty(second_node->occupied[1])) {
    first_node->children[byte2] = {};
    arena_->releaseNode(second);
  }
  if (bitmapEmpty(first_node->occupied[0]) &&
      bitmapEmpty(first_node->occupied[1])) {
    root_.children[byte3] = {};
    arena_->releaseNode(first);
  }
  return EraseStatus::Erased;
}

bool PriceLevelIndex::rollbackEnsure(uint32_t price, Side side,
                                     const EnsureResult &result) noexcept {
  return result.created() && result.handle.valid() &&
         eraseEmpty(price, side, result.handle) == EraseStatus::Erased;
}

void PriceLevelIndex::clear() noexcept {
  for (NodeHandle first : root_.children) {
    if (!first.valid()) {
      continue;
    }
    RadixNode *first_node = arena_->node(first);
    if (first_node == nullptr) {
      continue;
    }
    for (NodeHandle second : first_node->children) {
      if (!second.valid()) {
        continue;
      }
      RadixNode *second_node = arena_->node(second);
      if (second_node == nullptr) {
        continue;
      }
      for (NodeHandle raw_leaf : second_node->children) {
        const LeafHandle leaf_handle{raw_leaf.value};
        if (!leaf_handle.valid()) {
          continue;
        }
        PriceLeaf *leaf = arena_->leaf(leaf_handle);
        if (leaf != nullptr) {
          for (const auto &side_levels : leaf->levels) {
            for (PriceLevelHandle level_handle : side_levels) {
              if (level_handle.valid()) {
                arena_->releaseLevel(level_handle);
              }
            }
          }
        }
        arena_->releaseLeaf(leaf_handle);
      }
      arena_->releaseNode(second);
    }
    arena_->releaseNode(first);
  }
  root_.reset();
}

bool PriceLevelIndex::empty(Side side) const noexcept {
  return bitmapEmpty(root_.occupied[sideIndex(side)]);
}
