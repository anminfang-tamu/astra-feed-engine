#include "astra/book/PriceLevelStore.hpp"

#include "astra/symbol/StockLocate.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace astra::book {
namespace {

PriceLevelStoreConfig
validatedConfig(PriceLevelStoreConfig config) {
  if (config.page_capacity == 0 ||
      config.page_capacity ==
          std::numeric_limits<PricePageHandle>::max()) {
    throw std::invalid_argument(
        "PriceLevelStore page capacity must be in [1, UINT32_MAX-1]");
  }
  return config;
}

} // namespace

PriceLevelStore::PriceLevelStore(PriceLevelStoreConfig config)
    : config_(validatedConfig(config)),
      roots_(static_cast<std::size_t>(kPriceRootSlotCount),
             kHotArenaAlignment, "astra-price-roots"),
      prepared_books_(astra::symbol::kStockLocateSlots,
                      kHotArenaAlignment, "astra-price-prepared"),
      pages_(config_.page_capacity, kPricePageBytes,
             "astra-price-pages"),
      page_owners_(static_cast<std::size_t>(config_.page_capacity) + 1,
                   kHotArenaAlignment, "astra-price-owners"),
      page_summaries_(static_cast<std::size_t>(config_.page_capacity) + 1,
                      kHotArenaAlignment, "astra-price-summaries"),
      page_occupancy_((static_cast<std::size_t>(config_.page_capacity) + 1) *
                          kSideCount,
                      kHotArenaAlignment, "astra-price-page-bitmap"),
      book_summaries_(astra::symbol::kStockLocateSlots,
                      kHotArenaAlignment, "astra-price-book-summaries"),
      book_occupancy_(astra::symbol::kStockLocateSlots * kSideCount,
                      kHotArenaAlignment, "astra-price-book-bitmap") {
  static_assert(sizeof(std::size_t) >= sizeof(std::uint64_t),
                "the full price root requires a 64-bit process");
  if (config_.prefault) {
    roots_.prefault();
    prepared_books_.prefault();
    pages_.prefault();
    page_owners_.prefault();
    page_summaries_.prefault();
    page_occupancy_.prefault();
    book_summaries_.prefault();
    book_occupancy_.prefault();
  }
}

bool PriceLevelStore::validSide(OrderSide side) noexcept {
  return side == OrderSide::Buy || side == OrderSide::Sell;
}

std::uint32_t PriceLevelStore::sideIndex(OrderSide side) noexcept {
  return side == OrderSide::Buy ? 0u : 1u;
}

bool PriceLevelStore::better(OrderSide side, std::uint32_t lhs,
                             std::uint32_t rhs) noexcept {
  return side == OrderSide::Buy ? lhs > rhs : lhs < rhs;
}

std::uint64_t PriceLevelStore::pageSideIndex(PricePageHandle handle,
                                              OrderSide side) const noexcept {
  return static_cast<std::uint64_t>(handle) * kSideCount + sideIndex(side);
}

std::uint32_t
PriceLevelStore::bookSideIndex(std::uint16_t stock_locate,
                               OrderSide side) const noexcept {
  return static_cast<std::uint32_t>(stock_locate) * kSideCount +
         sideIndex(side);
}

PriceLevelStore::PageSideSummary *
PriceLevelStore::pageSummary(PricePageHandle handle,
                             OrderSide side) noexcept {
  return &page_summaries_[handle].sides[sideIndex(side)];
}

const PriceLevelStore::PageSideSummary *
PriceLevelStore::pageSummary(PricePageHandle handle,
                             OrderSide side) const noexcept {
  return &page_summaries_[handle].sides[sideIndex(side)];
}

PriceBitmapStorage *
PriceLevelStore::pageOccupancy(PricePageHandle handle,
                               OrderSide side) noexcept {
  return &page_occupancy_[
      static_cast<std::size_t>(pageSideIndex(handle, side))];
}

const PriceBitmapStorage *
PriceLevelStore::pageOccupancy(PricePageHandle handle,
                               OrderSide side) const noexcept {
  return &page_occupancy_[
      static_cast<std::size_t>(pageSideIndex(handle, side))];
}

PriceLevelStore::BookSideSummary *
PriceLevelStore::bookSummary(std::uint16_t stock_locate,
                             OrderSide side) noexcept {
  return &book_summaries_[stock_locate].sides[sideIndex(side)];
}

const PriceLevelStore::BookSideSummary *
PriceLevelStore::bookSummary(std::uint16_t stock_locate,
                             OrderSide side) const noexcept {
  return &book_summaries_[stock_locate].sides[sideIndex(side)];
}

PriceBitmapStorage *
PriceLevelStore::bookOccupancy(std::uint16_t stock_locate,
                               OrderSide side) noexcept {
  return &book_occupancy_[bookSideIndex(stock_locate, side)];
}

const PriceBitmapStorage *
PriceLevelStore::bookOccupancy(std::uint16_t stock_locate,
                               OrderSide side) const noexcept {
  return &book_occupancy_[bookSideIndex(stock_locate, side)];
}

const PricePage *PriceLevelStore::page(PricePageHandle handle) const noexcept {
  if (handle == kInvalidPricePageHandle || handle > committed_pages_)
    return nullptr;
  return &pages_[static_cast<std::size_t>(handle - 1)];
}

PriceLevelState *PriceLevelStore::levelUnchecked(PriceAddress address,
                                                 OrderSide side) noexcept {
  PricePage &price_page =
      pages_[static_cast<std::size_t>(address.page_handle - 1)];
  return side == OrderSide::Buy ? &price_page.bids[address.level_index]
                                : &price_page.asks[address.level_index];
}

const PriceLevelState *
PriceLevelStore::levelUnchecked(PriceAddress address,
                                OrderSide side) const noexcept {
  const PricePage &price_page =
      pages_[static_cast<std::size_t>(address.page_handle - 1)];
  return side == OrderSide::Buy ? &price_page.bids[address.level_index]
                                : &price_page.asks[address.level_index];
}

PriceLevelStore::PageOwner *
PriceLevelStore::owner(PricePageHandle handle) noexcept {
  if (handle == kInvalidPricePageHandle || handle > committed_pages_)
    return nullptr;
  return &page_owners_[handle];
}

const PriceLevelStore::PageOwner *
PriceLevelStore::owner(PricePageHandle handle) const noexcept {
  if (handle == kInvalidPricePageHandle || handle > committed_pages_)
    return nullptr;
  return &page_owners_[handle];
}

bool PriceLevelStore::rootHandleMatches(
    std::uint16_t stock_locate, std::uint16_t page_index,
    PricePageHandle handle) const noexcept {
  const PageOwner *page_owner = owner(handle);
  return page_owner != nullptr && page_owner->committed != 0 &&
         page_owner->stock_locate == stock_locate &&
         page_owner->page_index == page_index;
}

MutationResult
PriceLevelStore::prepareBook(std::uint16_t stock_locate) noexcept {
  if (!astra::symbol::isValidStockLocate(stock_locate))
    return MutationResult::InvalidLocate;
  if (prepared_books_[stock_locate] == 0) {
    prepared_books_[stock_locate] = 1;
    ++prepared_book_count_;
  }
  return MutationResult::Applied;
}

bool PriceLevelStore::isBookPrepared(
    std::uint16_t stock_locate) const noexcept {
  return astra::symbol::isValidStockLocate(stock_locate) &&
         prepared_books_[stock_locate] != 0;
}

PricePageHandle
PriceLevelStore::pageHandle(std::uint16_t stock_locate,
                            std::uint16_t page_index) const noexcept {
  if (!astra::symbol::isValidStockLocate(stock_locate))
    return kInvalidPricePageHandle;
  const PricePageHandle handle = roots_[static_cast<std::size_t>(
      priceRootIndex(stock_locate, page_index))];
  return rootHandleMatches(stock_locate, page_index, handle)
             ? handle
             : kInvalidPricePageHandle;
}

bool PriceLevelStore::ownsAddress(std::uint16_t stock_locate,
                                  std::uint16_t page_index,
                                  PriceAddress address) const noexcept {
  if (!astra::symbol::isValidStockLocate(stock_locate))
    return false;
  const PageOwner *page_owner = owner(address.page_handle);
  return page_owner != nullptr && page_owner->committed != 0 &&
         page_owner->stock_locate == stock_locate &&
         page_owner->page_index == page_index;
}

PricePageReservation PriceLevelStore::reservePrice(
    std::uint16_t stock_locate, std::uint32_t raw_price) const noexcept {
  PricePageReservation reservation{};
  if (!astra::symbol::isValidStockLocate(stock_locate)) {
    reservation.result = MutationResult::InvalidLocate;
    return reservation;
  }
  if (!isBookPrepared(stock_locate)) {
    reservation.result = MutationResult::BookNotPrepared;
    return reservation;
  }

  const PriceParts parts = splitPrice(raw_price);
  reservation.stock_locate = stock_locate;
  reservation.page_index = parts.page_index;
  reservation.address.level_index = parts.level_index;
  reservation.expected_committed_pages = committed_pages_;

  const PricePageHandle existing = roots_[static_cast<std::size_t>(
      priceRootIndex(stock_locate, parts.page_index))];
  if (existing != kInvalidPricePageHandle) {
    if (!rootHandleMatches(stock_locate, parts.page_index, existing)) {
      reservation.result = MutationResult::InvariantViolation;
      return reservation;
    }
    reservation.address.page_handle = existing;
    reservation.requires_commit = false;
    reservation.result = MutationResult::Applied;
    return reservation;
  }

  if (committed_pages_ >= config_.page_capacity) {
    ++page_capacity_failures_;
    reservation.result = MutationResult::PricePageCapacityExceeded;
    return reservation;
  }

  reservation.address.page_handle = committed_pages_ + 1;
  reservation.requires_commit = true;
  reservation.result = MutationResult::Applied;
  return reservation;
}

MutationResult PriceLevelStore::commitReservation(
    const PricePageReservation &reservation) noexcept {
  if (reservation.result != MutationResult::Applied)
    return reservation.result;
  if (!reservation.address.valid())
    return MutationResult::InvalidPricePage;
  if (!astra::symbol::isValidStockLocate(reservation.stock_locate))
    return MutationResult::InvalidLocate;
  if (!isBookPrepared(reservation.stock_locate))
    return MutationResult::BookNotPrepared;

  const std::uint64_t root_index =
      priceRootIndex(reservation.stock_locate, reservation.page_index);
  PricePageHandle &root = roots_[static_cast<std::size_t>(root_index)];

  if (!reservation.requires_commit) {
    return root == reservation.address.page_handle
               ? MutationResult::Applied
               : MutationResult::StaleReservation;
  }

  if (root != kInvalidPricePageHandle ||
      committed_pages_ != reservation.expected_committed_pages ||
      reservation.address.page_handle != committed_pages_ + 1 ||
      reservation.address.page_handle > config_.page_capacity) {
    return MutationResult::StaleReservation;
  }

  const PricePageHandle handle = reservation.address.page_handle;
  PageOwner &page_owner = page_owners_[handle];
  if (page_owner.committed != 0)
    return MutationResult::InvariantViolation;

  page_owner.stock_locate = reservation.stock_locate;
  page_owner.page_index = reservation.page_index;
  page_owner.committed = 1;
  committed_pages_ = handle;
  root = handle; // Publish only after the page descriptor is complete.
  return MutationResult::Applied;
}

MutationResult PriceLevelStore::validateAddress(PriceAddress address,
                                                 OrderSide side) const noexcept {
  if (!validSide(side))
    return MutationResult::InvalidSide;
  if (!address.valid() || address.page_handle > committed_pages_)
    return MutationResult::InvalidPricePage;
  return MutationResult::Applied;
}

PriceLevelState *PriceLevelStore::resolveLevel(PriceAddress address,
                                               OrderSide side) noexcept {
  if (validateAddress(address, side) != MutationResult::Applied)
    return nullptr;
  return levelUnchecked(address, side);
}

const PriceLevelState *
PriceLevelStore::resolveLevel(PriceAddress address,
                              OrderSide side) const noexcept {
  if (validateAddress(address, side) != MutationResult::Applied)
    return nullptr;
  return levelUnchecked(address, side);
}

MutationResult PriceLevelStore::validateActivation(
    PriceAddress address, OrderSide side) const noexcept {
  const PageOwner *page_owner = owner(address.page_handle);
  if (page_owner == nullptr || page_owner->committed == 0 ||
      !astra::symbol::isValidStockLocate(page_owner->stock_locate)) {
    return MutationResult::InvariantViolation;
  }
  const PageSideSummary &page_summary =
      *pageSummary(address.page_handle, side);
  const BookSideSummary &book_summary =
      *bookSummary(page_owner->stock_locate, side);
  if (page_summary.active_levels >= kPricePartCount ||
      (page_summary.active_levels != 0 &&
       page_summary.best_level_index >= kPricePartCount) ||
      (page_summary.active_levels == 0 &&
       book_summary.active_pages >= kPricePartCount)) {
    return MutationResult::InvariantViolation;
  }
  return MutationResult::Applied;
}

MutationResult PriceLevelStore::validateAdd(
    PriceAddress address, OrderSide side, std::uint32_t qty) const noexcept {
  const MutationResult address_result = validateAddress(address, side);
  if (address_result != MutationResult::Applied)
    return address_result;
  if (qty == 0)
    return MutationResult::InvalidQuantity;

  const PriceLevelState *level = levelUnchecked(address, side);
  if (level->order_count == std::numeric_limits<std::uint32_t>::max())
    return MutationResult::OrderCountOverflow;
  if (level->total_qty >
      std::numeric_limits<std::uint64_t>::max() -
          static_cast<std::uint64_t>(qty)) {
    return MutationResult::QuantityOverflow;
  }
  if ((level->order_count == 0) != (level->total_qty == 0))
    return MutationResult::InvariantViolation;
  if (level->order_count == 0)
    return validateActivation(address, side);
  return MutationResult::Applied;
}

MutationResult PriceLevelStore::validateErase(
    PriceAddress address, OrderSide side, std::uint32_t qty,
    DeactivationPlan &plan) const noexcept {
  plan = {};
  const MutationResult address_result = validateAddress(address, side);
  if (address_result != MutationResult::Applied)
    return address_result;
  if (qty == 0)
    return MutationResult::InvalidQuantity;

  const PriceLevelState *level = levelUnchecked(address, side);
  if (level->order_count == 0 || level->total_qty == 0)
    return MutationResult::MissingPriceLevel;
  if (level->total_qty < qty)
    return MutationResult::QuantityUnderflow;
  if (level->order_count == 1 && level->total_qty != qty)
    return MutationResult::InvariantViolation;
  if (level->order_count > 1 && level->total_qty == qty)
    return MutationResult::InvariantViolation;
  if (level->order_count == 1)
    return validateDeactivation(address, side, plan);
  return MutationResult::Applied;
}

void PriceLevelStore::activateLevel(PriceAddress address,
                                    OrderSide side) noexcept {
  const PageOwner &page_owner = page_owners_[address.page_handle];
  PageSideSummary &page_summary =
      *pageSummary(address.page_handle, side);
  PriceBitmapStorage &page_bitmap =
      *pageOccupancy(address.page_handle, side);
  BookSideSummary &book_summary =
      *bookSummary(page_owner.stock_locate, side);
  PriceBitmapStorage &book_bitmap =
      *bookOccupancy(page_owner.stock_locate, side);
  const bool page_was_empty = page_summary.active_levels == 0;
  const bool book_was_empty = book_summary.active_pages == 0;

  // A singleton page needs no level bitmap: its sole index is already cached
  // in the compact summary. Materialize both bits only on the 1 -> 2
  // transition; collapse the bitmap again on the reverse transition.
  if (page_summary.active_levels == 1) {
    PriceBitmap::set(page_bitmap, page_summary.best_level_index);
    PriceBitmap::set(page_bitmap, address.level_index);
  } else if (page_summary.active_levels > 1) {
    PriceBitmap::set(page_bitmap, address.level_index);
  }
  ++page_summary.active_levels;
  if (page_was_empty ||
      better(side, address.level_index, page_summary.best_level_index)) {
    page_summary.best_level_index = address.level_index;
  }

  if (page_was_empty) {
    PriceBitmap::set(book_bitmap, page_owner.page_index);
    ++book_summary.active_pages;
  }

  const std::uint32_t raw_price =
      joinPrice(page_owner.page_index, address.level_index);
  if (book_was_empty ||
      book_summary.best_page_handle == kInvalidPricePageHandle ||
      better(side, raw_price, book_summary.best_raw_price)) {
    book_summary.best_raw_price = raw_price;
    book_summary.best_page_handle = address.page_handle;
  }
}

void PriceLevelStore::addValidated(PriceAddress address, OrderSide side,
                                   std::uint32_t qty) noexcept {
  PriceLevelState &level = *levelUnchecked(address, side);
  const bool was_empty = level.order_count == 0;
  if (was_empty)
    activateLevel(address, side);
  level.total_qty += qty;
  ++level.order_count;
}

MutationResult PriceLevelStore::add(PriceAddress address, OrderSide side,
                                    std::uint32_t qty) noexcept {
  const MutationResult validation = validateAdd(address, side, qty);
  if (validation != MutationResult::Applied)
    return validation;
  addValidated(address, side, qty);
  return MutationResult::Applied;
}

MutationResult PriceLevelStore::reduce(PriceAddress address, OrderSide side,
                                       std::uint32_t current_order_qty,
                                       std::uint32_t reduction_qty) noexcept {
  const MutationResult address_result = validateAddress(address, side);
  if (address_result != MutationResult::Applied)
    return address_result;
  if (current_order_qty == 0 || reduction_qty == 0)
    return MutationResult::InvalidQuantity;
  if (reduction_qty >= current_order_qty)
    return MutationResult::QuantityUnderflow;

  PriceLevelState &level = *levelUnchecked(address, side);
  if (level.order_count == 0 || level.total_qty == 0)
    return MutationResult::MissingPriceLevel;
  if (level.total_qty < current_order_qty)
    return MutationResult::QuantityUnderflow;
  if (level.order_count == 1 && level.total_qty != current_order_qty)
    return MutationResult::InvariantViolation;
  if (level.order_count > 1 && level.total_qty == current_order_qty)
    return MutationResult::InvariantViolation;
  // reduce() represents a partial order mutation; it must not make a live
  // logical level quantity-free.  Final-order removal uses erase().
  if (level.total_qty <= reduction_qty)
    return MutationResult::QuantityUnderflow;
  level.total_qty -= reduction_qty;
  return MutationResult::Applied;
}

MutationResult PriceLevelStore::reduceChecked(
    std::uint16_t expected_stock_locate,
    std::uint16_t expected_page_index, PriceAddress address,
    OrderSide side, std::uint32_t current_order_qty,
    std::uint32_t reduction_qty) noexcept {
  if (!astra::symbol::isValidStockLocate(expected_stock_locate))
    return MutationResult::InvalidLocate;

  const MutationResult address_result = validateAddress(address, side);
  if (address_result != MutationResult::Applied)
    return address_result;

  const PageOwner &page_owner = page_owners_[address.page_handle];
  if (page_owner.committed == 0 ||
      page_owner.stock_locate != expected_stock_locate ||
      page_owner.page_index != expected_page_index) {
    return MutationResult::InvariantViolation;
  }
  if (current_order_qty == 0 || reduction_qty == 0)
    return MutationResult::InvalidQuantity;
  if (reduction_qty >= current_order_qty)
    return MutationResult::QuantityUnderflow;

  PriceLevelState &level = *levelUnchecked(address, side);
  if (level.order_count == 0 || level.total_qty == 0)
    return MutationResult::MissingPriceLevel;
  if (level.total_qty < current_order_qty)
    return MutationResult::QuantityUnderflow;
  if (level.order_count == 1 && level.total_qty != current_order_qty)
    return MutationResult::InvariantViolation;
  if (level.order_count > 1 && level.total_qty == current_order_qty)
    return MutationResult::InvariantViolation;
  if (level.total_qty <= reduction_qty)
    return MutationResult::QuantityUnderflow;
  level.total_qty -= reduction_qty;
  return MutationResult::Applied;
}

MutationResult PriceLevelStore::validateDeactivation(
    PriceAddress address, OrderSide side,
    DeactivationPlan &plan) const noexcept {
  plan = {};
  const PageOwner *page_owner = owner(address.page_handle);
  if (page_owner == nullptr || page_owner->committed == 0 ||
      !astra::symbol::isValidStockLocate(page_owner->stock_locate)) {
    return MutationResult::InvariantViolation;
  }

  const PageSideSummary &page_summary =
      *pageSummary(address.page_handle, side);
  const BookSideSummary &book_summary =
      *bookSummary(page_owner->stock_locate, side);
  const PriceBitmapStorage &book_bitmap =
      *bookOccupancy(page_owner->stock_locate, side);
  const std::uint32_t levels_before = page_summary.active_levels;
  if (levels_before == 0 || levels_before > kPricePartCount ||
      page_summary.best_level_index >= kPricePartCount ||
      book_summary.active_pages == 0 ||
      book_summary.active_pages > kPricePartCount ||
      book_summary.best_page_handle == kInvalidPricePageHandle ||
      !PriceBitmap::test(book_bitmap, page_owner->page_index)) {
    return MutationResult::InvariantViolation;
  }

  const PriceParts cached_best = splitPrice(book_summary.best_raw_price);
  const PageOwner *cached_owner = owner(book_summary.best_page_handle);
  if (cached_owner == nullptr || cached_owner->committed == 0 ||
      cached_owner->stock_locate != page_owner->stock_locate ||
      cached_owner->page_index != cached_best.page_index) {
    return MutationResult::InvariantViolation;
  }
  const PageSideSummary &cached_page =
      *pageSummary(book_summary.best_page_handle, side);
  if (cached_page.active_levels == 0 ||
      cached_page.best_level_index != cached_best.level_index) {
    return MutationResult::InvariantViolation;
  }
  const PriceLevelState &cached_level = *levelUnchecked(
      PriceAddress{book_summary.best_page_handle, cached_best.level_index, 0},
      side);
  if (cached_level.order_count == 0 || cached_level.total_qty == 0)
    return MutationResult::InvariantViolation;

  const bool removed_is_page_best =
      page_summary.best_level_index == address.level_index;
  if (levels_before == 1) {
    // The 0 -> 1 transition never materializes a page-level bitmap, and the
    // 2 -> 1 transition clears both remaining bits.  The compact summary is
    // therefore authoritative for a singleton page.  Do not pull the cold
    // 8 KiB page bitmap into cache merely to re-check that construction
    // invariant on the hot final-order removal path.
    if (!removed_is_page_best)
      return MutationResult::InvariantViolation;
    plan.page_becomes_empty = true;
  } else {
    const PriceBitmapStorage &page_bitmap =
        *pageOccupancy(address.page_handle, side);
    if (PriceBitmap::empty(page_bitmap) ||
        !PriceBitmap::test(page_bitmap, address.level_index) ||
        !PriceBitmap::test(page_bitmap,
                           page_summary.best_level_index)) {
      return MutationResult::InvariantViolation;
    }
    if (removed_is_page_best) {
      if (side == OrderSide::Buy) {
        if (address.level_index != 0) {
          plan.next_level_index = PriceBitmap::findHighestAtOrBelow(
              page_bitmap,
              static_cast<std::uint32_t>(address.level_index) - 1);
        }
      } else if (address.level_index !=
                 std::numeric_limits<std::uint16_t>::max()) {
        plan.next_level_index = PriceBitmap::findLowestAtOrAbove(
            page_bitmap,
            static_cast<std::uint32_t>(address.level_index) + 1);
      }
      if (plan.next_level_index == PriceBitmap::kInvalid)
        return MutationResult::InvariantViolation;
      const PriceLevelState &next_level = *levelUnchecked(
          PriceAddress{address.page_handle,
                       static_cast<std::uint16_t>(plan.next_level_index), 0},
          side);
      if (next_level.order_count == 0 || next_level.total_qty == 0)
        return MutationResult::InvariantViolation;
      plan.page_best_changes = true;
    }
  }

  const std::uint32_t removed_raw =
      joinPrice(page_owner->page_index, address.level_index);
  const bool removed_is_book_best =
      book_summary.best_raw_price == removed_raw;
  if (removed_is_book_best) {
    if (book_summary.best_page_handle != address.page_handle ||
        !removed_is_page_best) {
      return MutationResult::InvariantViolation;
    }
    plan.book_best_changes = true;
    if (!plan.page_becomes_empty) {
      if (!plan.page_best_changes)
        return MutationResult::InvariantViolation;
      plan.next_book_raw_price = joinPrice(
          page_owner->page_index,
          static_cast<std::uint16_t>(plan.next_level_index));
      plan.next_book_page_handle = address.page_handle;
      return MutationResult::Applied;
    }

    std::uint32_t next_page_index = PriceBitmap::kInvalid;
    if (side == OrderSide::Buy) {
      if (page_owner->page_index != 0) {
        next_page_index = PriceBitmap::findHighestAtOrBelow(
            book_bitmap,
            static_cast<std::uint32_t>(page_owner->page_index) - 1);
      }
    } else if (page_owner->page_index !=
               std::numeric_limits<std::uint16_t>::max()) {
      next_page_index = PriceBitmap::findLowestAtOrAbove(
          book_bitmap,
          static_cast<std::uint32_t>(page_owner->page_index) + 1);
    }

    if (book_summary.active_pages == 1) {
      return next_page_index == PriceBitmap::kInvalid
                 ? MutationResult::Applied
                 : MutationResult::InvariantViolation;
    }
    if (next_page_index == PriceBitmap::kInvalid)
      return MutationResult::InvariantViolation;

    const PricePageHandle next_handle = pageHandle(
        page_owner->stock_locate,
        static_cast<std::uint16_t>(next_page_index));
    if (next_handle == kInvalidPricePageHandle)
      return MutationResult::InvariantViolation;
    const PageSideSummary &next_page = *pageSummary(next_handle, side);
    if (next_page.active_levels == 0 ||
        next_page.best_level_index >= kPricePartCount) {
      return MutationResult::InvariantViolation;
    }
    const PriceLevelState &next_level = *levelUnchecked(
        PriceAddress{next_handle,
                     static_cast<std::uint16_t>(
                         next_page.best_level_index),
                     0},
        side);
    if (next_level.order_count == 0 || next_level.total_qty == 0)
      return MutationResult::InvariantViolation;
    plan.next_book_raw_price = joinPrice(
        static_cast<std::uint16_t>(next_page_index),
        static_cast<std::uint16_t>(next_page.best_level_index));
    plan.next_book_page_handle = next_handle;
  } else if (plan.page_becomes_empty &&
             book_summary.active_pages == 1) {
    return MutationResult::InvariantViolation;
  }
  return MutationResult::Applied;
}

void PriceLevelStore::deactivateLevel(
    PriceAddress address, OrderSide side,
    const DeactivationPlan &plan) noexcept {
  const PageOwner &page_owner = page_owners_[address.page_handle];
  PageSideSummary &page_summary = *pageSummary(address.page_handle, side);
  PriceBitmapStorage &page_bitmap =
      *pageOccupancy(address.page_handle, side);
  BookSideSummary &book_summary =
      *bookSummary(page_owner.stock_locate, side);
  PriceBitmapStorage &book_bitmap =
      *bookOccupancy(page_owner.stock_locate, side);

  if (plan.page_becomes_empty) {
    page_summary.active_levels = 0;
    page_summary.best_level_index = 0;
    PriceBitmap::reset(book_bitmap, page_owner.page_index);
    --book_summary.active_pages;
  } else {
    PriceBitmap::reset(page_bitmap, address.level_index);
    --page_summary.active_levels;
    if (plan.page_best_changes)
      page_summary.best_level_index = plan.next_level_index;
    if (page_summary.active_levels == 1)
      PriceBitmap::reset(page_bitmap, page_summary.best_level_index);
  }

  if (plan.book_best_changes) {
    book_summary.best_raw_price = plan.next_book_raw_price;
    book_summary.best_page_handle = plan.next_book_page_handle;
  }
}

void PriceLevelStore::eraseValidated(
    PriceAddress address, OrderSide side, std::uint32_t qty,
    const DeactivationPlan &plan) noexcept {
  PriceLevelState &level = *levelUnchecked(address, side);
  const bool level_becomes_empty = level.order_count == 1;
  level.total_qty -= qty;
  --level.order_count;
  if (level_becomes_empty)
    deactivateLevel(address, side, plan);
}

MutationResult PriceLevelStore::erase(
    PriceAddress address, OrderSide side,
    std::uint32_t remaining_order_qty) noexcept {
  DeactivationPlan plan{};
  const MutationResult validation =
      validateErase(address, side, remaining_order_qty, plan);
  if (validation != MutationResult::Applied)
    return validation;
  eraseValidated(address, side, remaining_order_qty, plan);
  return MutationResult::Applied;
}

MutationResult PriceLevelStore::move(PriceAddress old_address,
                                     std::uint32_t old_qty,
                                     PriceAddress new_address,
                                     std::uint32_t new_qty,
                                     OrderSide side) noexcept {
  if (new_qty == 0)
    return MutationResult::InvalidQuantity;

  const PageOwner *old_owner = owner(old_address.page_handle);
  const PageOwner *new_owner = owner(new_address.page_handle);
  if (old_owner == nullptr || new_owner == nullptr)
    return MutationResult::InvalidPricePage;
  if (old_owner->committed == 0 || new_owner->committed == 0)
    return MutationResult::InvariantViolation;
  if (old_owner->stock_locate != new_owner->stock_locate)
    return MutationResult::LocateMismatch;

  DeactivationPlan source_plan{};
  const MutationResult source_validation =
      validateErase(old_address, side, old_qty, source_plan);
  if (source_validation != MutationResult::Applied)
    return source_validation;

  if (old_address.page_handle == new_address.page_handle &&
      old_address.level_index == new_address.level_index) {
    PriceLevelState &level = *levelUnchecked(old_address, side);
    const std::uint64_t without_old = level.total_qty - old_qty;
    if (without_old > std::numeric_limits<std::uint64_t>::max() - new_qty)
      return MutationResult::QuantityOverflow;
    level.total_qty = without_old + new_qty;
    return MutationResult::Applied;
  }

  const MutationResult destination_validation =
      validateAdd(new_address, side, new_qty);
  if (destination_validation != MutationResult::Applied)
    return destination_validation;

  // Both logical changes were preflighted against the same single-writer
  // state. Their commit helpers contain no rejection path, so a failed move
  // cannot strand the source after a partial mutation.
  eraseValidated(old_address, side, old_qty, source_plan);
  addValidated(new_address, side, new_qty);
  return MutationResult::Applied;
}

PriceLevelCursor PriceLevelStore::best(std::uint16_t stock_locate,
                                       OrderSide side) const noexcept {
  if (!isBookPrepared(stock_locate) || !validSide(side))
    return {};
  const BookSideSummary &book_summary = *bookSummary(stock_locate, side);
  if (book_summary.active_pages == 0 ||
      book_summary.best_page_handle == kInvalidPricePageHandle)
    return {};
  return PriceLevelCursor{book_summary.best_page_handle,
                          book_summary.best_raw_price, stock_locate, true};
}

PriceLevelCursor PriceLevelStore::nextWorse(
    std::uint16_t stock_locate, OrderSide side,
    PriceLevelCursor current) const noexcept {
  if (!current.valid || current.stock_locate != stock_locate ||
      !isBookPrepared(stock_locate) || !validSide(side))
    return {};

  const PriceParts parts = splitPrice(current.raw_price);
  const PageOwner *page_owner = owner(current.page_handle);
  if (page_owner == nullptr || page_owner->committed == 0 ||
      page_owner->stock_locate != stock_locate ||
      page_owner->page_index != parts.page_index) {
    return {};
  }

  const PageSideSummary &page_summary =
      *pageSummary(current.page_handle, side);
  std::uint32_t next_level_index = PriceBitmap::kInvalid;
  if (page_summary.active_levels > 1) {
    const PriceBitmapStorage &page_bitmap =
        *pageOccupancy(current.page_handle, side);
    if (side == OrderSide::Buy) {
      if (parts.level_index != 0) {
        next_level_index = PriceBitmap::findHighestAtOrBelow(
            page_bitmap,
            static_cast<std::uint32_t>(parts.level_index) - 1);
      }
    } else if (parts.level_index !=
               std::numeric_limits<std::uint16_t>::max()) {
      next_level_index = PriceBitmap::findLowestAtOrAbove(
          page_bitmap,
          static_cast<std::uint32_t>(parts.level_index) + 1);
    }
  }

  if (next_level_index != PriceBitmap::kInvalid) {
    return PriceLevelCursor{
        current.page_handle,
        joinPrice(parts.page_index,
                  static_cast<std::uint16_t>(next_level_index)),
        stock_locate, true};
  }

  const PriceBitmapStorage &book_bitmap =
      *bookOccupancy(stock_locate, side);
  std::uint32_t next_page_index = PriceBitmap::kInvalid;
  if (side == OrderSide::Buy) {
    if (parts.page_index != 0) {
      next_page_index = PriceBitmap::findHighestAtOrBelow(
          book_bitmap,
          static_cast<std::uint32_t>(parts.page_index) - 1);
    }
  } else if (parts.page_index !=
             std::numeric_limits<std::uint16_t>::max()) {
    next_page_index = PriceBitmap::findLowestAtOrAbove(
        book_bitmap,
        static_cast<std::uint32_t>(parts.page_index) + 1);
  }
  if (next_page_index == PriceBitmap::kInvalid)
    return {};

  const PricePageHandle next_handle =
      pageHandle(stock_locate, static_cast<std::uint16_t>(next_page_index));
  if (next_handle == kInvalidPricePageHandle)
    return {};
  const PageSideSummary &next_page = *pageSummary(next_handle, side);
  if (next_page.active_levels == 0 ||
      next_page.best_level_index >= kPricePartCount)
    return {};
  return PriceLevelCursor{
      next_handle,
      joinPrice(static_cast<std::uint16_t>(next_page_index),
                static_cast<std::uint16_t>(
                    next_page.best_level_index)),
      stock_locate, true};
}

PriceLevelView PriceLevelStore::view(PriceLevelCursor cursor,
                                     OrderSide side) const noexcept {
  if (!cursor.valid)
    return {};
  const PriceParts parts = splitPrice(cursor.raw_price);
  const PageOwner *page_owner = owner(cursor.page_handle);
  if (page_owner == nullptr || page_owner->committed == 0 ||
      !astra::symbol::isValidStockLocate(cursor.stock_locate) ||
      page_owner->stock_locate != cursor.stock_locate ||
      page_owner->page_index != parts.page_index)
    return {};
  const PriceLevelState *level =
      resolveLevel(
          PriceAddress{cursor.page_handle, parts.level_index, 0}, side);
  if (level == nullptr || level->order_count == 0)
    return {};
  return PriceLevelView{cursor.raw_price, level->total_qty,
                        level->order_count, true};
}

std::uint8_t PriceLevelStore::topTen(
    std::uint16_t stock_locate, OrderSide side,
    std::span<PriceLevelView> output) const noexcept {
  const std::size_t limit =
      std::min<std::size_t>(output.size(), kTopLevelCount);
  std::size_t count = 0;
  PriceLevelCursor cursor = best(stock_locate, side);
  while (cursor.valid && count < limit) {
    PriceLevelView level_view = view(cursor, side);
    if (!level_view.valid)
      break;
    output[count++] = level_view;
    // Do not search for an eleventh level after the caller-visible top-ten is
    // already complete. A full side therefore needs at most nine successor
    // traversals, not ten.
    if (count == limit)
      break;
    cursor = nextWorse(stock_locate, side, cursor);
  }
  return static_cast<std::uint8_t>(count);
}

PriceLevelStoreCounters PriceLevelStore::counters() const noexcept {
  return PriceLevelStoreCounters{config_.page_capacity, committed_pages_,
                                 prepared_book_count_,
                                 page_capacity_failures_};
}

} // namespace astra::book
