#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

class PriceLevelIndex;

// A 32-bit handle keeps Orders and radix leaves compact. Zero is always
// invalid, so a zero-filled node or leaf is ready for use without a separate
// initialization bitmap.
struct PriceLevelHandle {
  uint32_t value{0};

  constexpr bool valid() const noexcept { return value != 0; }
  constexpr explicit operator bool() const noexcept { return valid(); }
  friend constexpr bool operator==(PriceLevelHandle,
                                   PriceLevelHandle) noexcept = default;
};

static_assert(sizeof(PriceLevelHandle) == sizeof(uint32_t));

// The pooled level record deliberately keeps aggregate quantity at 64 bits.
// A busy level can exceed UINT32_MAX even though every individual ITCH order
// quantity is 32-bit.
struct alignas(8) PooledPriceLevel {
  static constexpr uint32_t kInvalidOrderIndex =
      std::numeric_limits<uint32_t>::max();

  uint32_t head_idx{kInvalidOrderIndex};
  uint32_t tail_idx{kInvalidOrderIndex};
  uint64_t total_qty{0};
  uint32_t num_orders{0};
  uint32_t reserved{0};

  bool empty() const noexcept {
    return num_orders == 0 && total_qty == 0 &&
           head_idx == kInvalidOrderIndex && tail_idx == kInvalidOrderIndex;
  }
};

static_assert(sizeof(PooledPriceLevel) == 24);

struct PriceLevelArenaConfig {
  // Internal nodes exclude the root embedded in each PriceLevelIndex. A
  // populated price path consumes at most two internal nodes, one leaf, and
  // one level. Bid and ask share nodes/leaves but have distinct levels.
  size_t internal_node_capacity{0};
  size_t leaf_capacity{0};
  size_t level_capacity{0};
};

struct PriceLevelArenaStats {
  size_t internal_node_capacity{0};
  size_t internal_nodes_in_use{0};
  size_t internal_node_high_watermark{0};
  size_t leaf_capacity{0};
  size_t leaves_in_use{0};
  size_t leaf_high_watermark{0};
  size_t level_capacity{0};
  size_t levels_in_use{0};
  size_t level_high_watermark{0};
  uint64_t internal_node_exhaustions{0};
  uint64_t leaf_exhaustions{0};
  uint64_t level_exhaustions{0};
};

// Owns the fixed, preallocated pools shared by all books. Construction
// allocates, initializes, and touches the complete configured storage. None of
// the hot operations allocate. The arena and its indexes are intentionally
// single-writer; the market-data engine owns them on its decode thread.
class PriceLevelArena {
public:
  explicit PriceLevelArena(PriceLevelArenaConfig config);

  PriceLevelArena(const PriceLevelArena &) = delete;
  PriceLevelArena &operator=(const PriceLevelArena &) = delete;
  PriceLevelArena(PriceLevelArena &&) = delete;
  PriceLevelArena &operator=(PriceLevelArena &&) = delete;

  PooledPriceLevel *level(PriceLevelHandle handle) noexcept;
  const PooledPriceLevel *level(PriceLevelHandle handle) const noexcept;

  PriceLevelArenaStats stats() const noexcept;
  size_t availableInternalNodes() const noexcept;
  size_t availableLeaves() const noexcept;
  size_t availableLevels() const noexcept;

private:
  friend class PriceLevelIndex;

  struct NodeHandle {
    uint32_t value{0};
    constexpr bool valid() const noexcept { return value != 0; }
  };

  struct LeafHandle {
    uint32_t value{0};
    constexpr bool valid() const noexcept { return value != 0; }
  };

  // Both sides share each child. The side-specific occupancy masks say
  // whether that child contains at least one price for the selected side.
  struct alignas(64) RadixNode {
    std::array<std::array<uint64_t, 4>, 2> occupied{};
    std::array<NodeHandle, 256> children{};

    void reset() noexcept;
  };

  struct alignas(64) PriceLeaf {
    std::array<std::array<uint64_t, 4>, 2> occupied{};
    std::array<std::array<PriceLevelHandle, 256>, 2> levels{};

    void reset() noexcept;
  };

  static_assert(sizeof(NodeHandle) == sizeof(uint32_t));
  static_assert(sizeof(LeafHandle) == sizeof(uint32_t));
  static_assert(sizeof(RadixNode) == 1088);
  static_assert(sizeof(PriceLeaf) == 2112);

  NodeHandle allocateNode() noexcept;
  LeafHandle allocateLeaf() noexcept;
  PriceLevelHandle allocateLevel() noexcept;
  void releaseNode(NodeHandle handle) noexcept;
  void releaseLeaf(LeafHandle handle) noexcept;
  void releaseLevel(PriceLevelHandle handle) noexcept;

  RadixNode *node(NodeHandle handle) noexcept;
  const RadixNode *node(NodeHandle handle) const noexcept;
  PriceLeaf *leaf(LeafHandle handle) noexcept;
  const PriceLeaf *leaf(LeafHandle handle) const noexcept;

  void recordInternalNodeExhaustion() noexcept {
    ++internal_node_exhaustions_;
  }
  void recordLeafExhaustion() noexcept { ++leaf_exhaustions_; }
  void recordLevelExhaustion() noexcept { ++level_exhaustions_; }

  std::vector<RadixNode> nodes_;
  std::vector<PriceLeaf> leaves_;
  std::vector<PooledPriceLevel> levels_;
  std::vector<uint32_t> free_nodes_;
  std::vector<uint32_t> free_leaves_;
  std::vector<uint32_t> free_levels_;
  size_t next_node_{0};
  size_t next_leaf_{0};
  size_t next_level_{0};
  size_t free_node_count_{0};
  size_t free_leaf_count_{0};
  size_t free_level_count_{0};
  size_t node_high_watermark_{0};
  size_t leaf_high_watermark_{0};
  size_t level_high_watermark_{0};
  uint64_t internal_node_exhaustions_{0};
  uint64_t leaf_exhaustions_{0};
  uint64_t level_exhaustions_{0};
};

class PriceLevelIndex {
public:
  enum class Side : uint8_t { Bid = 0, Ask = 1 };

  enum class EnsureStatus : uint8_t {
    Found,
    Created,
    InternalNodePoolExhausted,
    LeafPoolExhausted,
    LevelPoolExhausted,
  };

  struct EnsureResult {
    EnsureStatus status{EnsureStatus::LevelPoolExhausted};
    PriceLevelHandle handle{};

    bool ok() const noexcept {
      return status == EnsureStatus::Found || status == EnsureStatus::Created;
    }
    bool created() const noexcept { return status == EnsureStatus::Created; }
  };

  enum class EraseStatus : uint8_t {
    Erased,
    NotFound,
    HandleMismatch,
    LevelNotEmpty,
  };

  struct PricePoint {
    uint32_t price{0};
    PriceLevelHandle handle{};

    bool found() const noexcept { return handle.valid(); }
    constexpr explicit operator bool() const noexcept { return handle.valid(); }
  };

  explicit PriceLevelIndex(PriceLevelArena &arena) noexcept;
  ~PriceLevelIndex();

  PriceLevelIndex(const PriceLevelIndex &) = delete;
  PriceLevelIndex &operator=(const PriceLevelIndex &) = delete;
  PriceLevelIndex(PriceLevelIndex &&) = delete;
  PriceLevelIndex &operator=(PriceLevelIndex &&) = delete;

  // Finds or creates the side-specific level for the complete uint32 price
  // domain. Creation is atomic with respect to pool exhaustion: every needed
  // slot is preflighted before the first tree pointer or occupancy bit changes.
  EnsureResult ensure(uint32_t price, Side side) noexcept;
  PriceLevelHandle find(uint32_t price, Side side) const noexcept;

  // Best is highest for bids and lowest for asks. nextWorse is strict: it
  // returns the next lower bid or next higher ask.
  PricePoint best(Side side) const noexcept;
  PricePoint nextWorse(Side side, uint32_t price) const noexcept;

  // Empty-level erasure releases the level and recursively releases an empty
  // leaf/internal path. expected may be invalid to skip identity checking.
  EraseStatus eraseEmpty(uint32_t price, Side side,
                         PriceLevelHandle expected = {}) noexcept;

  // Intended for rolling back a successful ensure when a later order-pool
  // operation fails. It only removes a level created by that ensure and only
  // while the record is still empty.
  bool rollbackEnsure(uint32_t price, Side side,
                      const EnsureResult &result) noexcept;

  void clear() noexcept;
  bool empty(Side side) const noexcept;

private:
  using NodeHandle = PriceLevelArena::NodeHandle;
  using LeafHandle = PriceLevelArena::LeafHandle;
  using RadixNode = PriceLevelArena::RadixNode;
  using PriceLeaf = PriceLevelArena::PriceLeaf;

  static size_t sideIndex(Side side) noexcept {
    return static_cast<size_t>(side);
  }

  static bool bitmapEmpty(const std::array<uint64_t, 4> &bits) noexcept;
  static bool bitmapTest(const std::array<uint64_t, 4> &bits,
                         uint8_t index) noexcept;
  static void bitmapSet(std::array<uint64_t, 4> &bits,
                        uint8_t index) noexcept;
  static void bitmapClear(std::array<uint64_t, 4> &bits,
                          uint8_t index) noexcept;
  static int bitmapFirst(const std::array<uint64_t, 4> &bits) noexcept;
  static int bitmapLast(const std::array<uint64_t, 4> &bits) noexcept;
  static int bitmapNext(const std::array<uint64_t, 4> &bits,
                        uint8_t index) noexcept;
  static int bitmapPrevious(const std::array<uint64_t, 4> &bits,
                            uint8_t index) noexcept;

  PricePoint descendBest(NodeHandle first, uint8_t byte3, Side side,
                         bool high) const noexcept;
  PricePoint descendFromSecond(NodeHandle second, uint8_t byte3,
                               uint8_t byte2, Side side,
                               bool high) const noexcept;
  PricePoint pointFromLeaf(LeafHandle leaf, uint8_t byte3, uint8_t byte2,
                           uint8_t byte1, Side side,
                           bool high) const noexcept;

  PriceLevelArena *arena_;
  RadixNode root_{};
};
