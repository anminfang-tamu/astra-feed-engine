#include "astra/book/OrderBook.hpp"

#include "astra/protocol/ItchPrice.hpp"
#include "astra/symbol/StockLocate.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>

namespace {

constexpr std::uint64_t nextPowerOfTwo(std::uint64_t value) noexcept {
  if (value <= 2)
    return 2;
  --value;
  value |= value >> 1;
  value |= value >> 2;
  value |= value >> 4;
  value |= value >> 8;
  value |= value >> 16;
  value |= value >> 32;
  return value + 1;
}

astra::book::OrderTableConfig
standaloneOrderConfig(std::size_t requested_capacity) {
  constexpr std::uint64_t kMaximumStandaloneCapacity =
      std::uint64_t{1} << 28;
  if (requested_capacity > kMaximumStandaloneCapacity) {
    throw std::invalid_argument(
        "standalone OrderBook capacity exceeds its supported test limit");
  }
  const std::uint64_t safe_capacity = requested_capacity;
  const std::uint64_t direct =
      nextPowerOfTwo(std::max<std::uint64_t>(1024, safe_capacity * 4));
  const std::uint64_t fallback_target =
      nextPowerOfTwo(std::max<std::uint64_t>(64, safe_capacity / 4));
  return {direct, static_cast<std::uint32_t>(fallback_target), false};
}

astra::book::PriceLevelStoreConfig
standalonePriceConfig(std::size_t requested_capacity) noexcept {
  const std::uint64_t requested_pages =
      std::max<std::uint64_t>(1, requested_capacity);
  // One standalone book has one locate and therefore cannot use more than one
  // page for each of the 65,536 page indices in the uint32 price domain.
  const std::uint64_t maximum_pages = astra::book::kPricePartCount;
  return {static_cast<std::uint32_t>(
              std::min(requested_pages, maximum_pages)),
          false};
}

} // namespace

struct OrderBook::OwnedStores {
  explicit OwnedStores(std::size_t requested_capacity)
      : orders(standaloneOrderConfig(requested_capacity)),
        prices(standalonePriceConfig(requested_capacity)) {}

  astra::book::OrderTable orders;
  astra::book::PriceLevelStore prices;
};

OrderBook::OrderBook(std::uint32_t symbol_id, std::size_t order_capacity)
    : owned_stores_(std::make_unique<OwnedStores>(order_capacity)),
      orders_(&owned_stores_->orders), prices_(&owned_stores_->prices),
      symbol_id_(symbol_id),
      stock_locate_(static_cast<std::uint16_t>(symbol_id)),
      order_capacity_(order_capacity) {
  if (symbol_id > std::numeric_limits<std::uint16_t>::max() ||
      !astra::symbol::isValidStockLocate(stock_locate_) ||
      prices_->prepareBook(stock_locate_) !=
          astra::book::MutationResult::Applied) {
    local_invalid_ = true;
  }
}

OrderBook::OrderBook(std::uint16_t stock_locate,
                     astra::book::OrderTable &orders,
                     astra::book::PriceLevelStore &prices,
                     std::size_t reported_capacity) noexcept
    : orders_(&orders), prices_(&prices), symbol_id_(stock_locate),
      stock_locate_(stock_locate), order_capacity_(reported_capacity) {
  if (!prices_->isBookPrepared(stock_locate_))
    local_invalid_ = true;
}

OrderBook::~OrderBook() = default;

OrderSide OrderBook::decodeSide(char side) noexcept {
  if (side == 'B')
    return OrderSide::Buy;
  if (side == 'S')
    return OrderSide::Sell;
  return static_cast<OrderSide>(0);
}

OrderSide OrderBook::decodeSide(std::uint8_t side) noexcept {
  const auto decoded = static_cast<OrderSide>(side);
  return decoded == OrderSide::Buy || decoded == OrderSide::Sell
             ? decoded
             : static_cast<OrderSide>(0);
}

astra::book::PriceAddress
OrderBook::addressOf(const astra::book::OrderState &state) noexcept {
  return {state.price_page_handle, state.price_level_index, 0};
}

astra::book::MutationResult
OrderBook::reject(astra::book::MutationResult result) noexcept {
  if (result != astra::book::MutationResult::Applied) {
    if (result == astra::book::MutationResult::OrderTableCapacityExceeded ||
        result == astra::book::MutationResult::PricePageCapacityExceeded) {
      ++allocation_failures_;
    }
    if (result == astra::book::MutationResult::QuantityOverflow ||
        result == astra::book::MutationResult::QuantityUnderflow ||
        result == astra::book::MutationResult::InvalidPricePage ||
        result == astra::book::MutationResult::MissingPriceLevel ||
        result == astra::book::MutationResult::StaleReservation ||
        result == astra::book::MutationResult::InvariantViolation ||
        result == astra::book::MutationResult::BookInvalid) {
      local_invalid_ = true;
    }
  }
  return result;
}

astra::book::MutationResult
OrderBook::validateState(astra::book::OrderState *state) noexcept {
  if (state == nullptr)
    return reject(astra::book::MutationResult::OrderNotFound);
  if (state->locate != stock_locate_)
    return reject(astra::book::MutationResult::LocateMismatch);
  if (decodeSide(state->side) == static_cast<OrderSide>(0) ||
      state->qty == 0 ||
      state->price_page_handle ==
          astra::book::kInvalidPricePageHandle) {
    return reject(astra::book::MutationResult::InvariantViolation);
  }
  if (!prices_->ownsAddress(stock_locate_, state->price_page_index,
                            addressOf(*state)))
    return reject(astra::book::MutationResult::InvariantViolation);
  return astra::book::MutationResult::Applied;
}

astra::book::MutationResult
OrderBook::addOrder(std::uint64_t order_id, std::uint64_t price,
                    std::uint32_t qty, char side,
                    std::uint64_t timestamp) noexcept {
  if (local_invalid_)
    return astra::book::MutationResult::BookInvalid;
  if (orders_ == nullptr || prices_ == nullptr ||
      !prices_->isBookPrepared(stock_locate_)) {
    return reject(astra::book::MutationResult::BookNotPrepared);
  }
  const OrderSide decoded_side = decodeSide(side);
  if (decoded_side == static_cast<OrderSide>(0))
    return reject(astra::book::MutationResult::InvalidSide);
  if (qty == 0)
    return reject(astra::book::MutationResult::InvalidQuantity);
  if (!astra::protocol::isValidItchPrice4(price))
    return reject(astra::book::MutationResult::PriceOutOfRange);

  const astra::book::OrderSlotReservation order_reservation =
      orders_->reserve(order_id);
  if (!order_reservation.valid())
    return reject(order_reservation.result);

  const auto price_reservation = prices_->reservePrice(
      stock_locate_, static_cast<std::uint32_t>(price));
  if (!price_reservation.valid())
    return reject(price_reservation.result);
  if (price_reservation.requires_commit) {
    const auto page_commit = prices_->commitReservation(price_reservation);
    if (page_commit != astra::book::MutationResult::Applied)
      return reject(page_commit);
  }

  const auto level_result =
      prices_->add(price_reservation.address, decoded_side, qty);
  if (level_result != astra::book::MutationResult::Applied)
    return reject(level_result);

  astra::book::OrderState state{
      qty,
      price_reservation.address.page_handle,
      price_reservation.address.level_index,
      stock_locate_,
      static_cast<std::uint8_t>(decoded_side),
      0,
      price_reservation.page_index};
  const auto order_commit = orders_->commit(order_reservation, state);
  if (order_commit != astra::book::MutationResult::Applied) {
    const auto rollback =
        prices_->erase(price_reservation.address, decoded_side, qty);
    return reject(rollback == astra::book::MutationResult::Applied
                      ? order_commit
                      : astra::book::MutationResult::InvariantViolation);
  }

  ++live_order_count_;
  latest_update_timestamp_ = timestamp;
  return astra::book::MutationResult::Applied;
}

astra::book::MutationResult
OrderBook::cancelShares(std::uint64_t order_id,
                        std::uint32_t canceled_qty,
                        std::uint64_t timestamp) noexcept {
  if (local_invalid_)
    return astra::book::MutationResult::BookInvalid;
  if (canceled_qty == 0)
    return reject(astra::book::MutationResult::InvalidQuantity);
  astra::book::OrderState *const state = orders_->findState(order_id);
  if (state == nullptr)
    return reject(astra::book::MutationResult::OrderNotFound);

  const std::uint16_t state_locate = state->locate;
  const std::uint16_t state_page_index = state->price_page_index;
  const OrderSide side = decodeSide(state->side);
  const std::uint32_t current_qty = state->qty;
  const astra::book::PriceAddress address{
      state->price_page_handle, state->price_level_index, 0};
  if (state_locate != stock_locate_)
    return reject(astra::book::MutationResult::LocateMismatch);
  if (side == static_cast<OrderSide>(0) || current_qty == 0 ||
      !address.valid()) {
    return reject(astra::book::MutationResult::InvariantViolation);
  }
  // ITCH X is a partial cancel.  D removes the final order.
  if (canceled_qty >= current_qty) {
    if (!prices_->ownsAddress(stock_locate_, state_page_index, address))
      return reject(astra::book::MutationResult::InvariantViolation);
    return reject(astra::book::MutationResult::QuantityExceeded);
  }

  const std::uint32_t remaining_qty = current_qty - canceled_qty;
  const auto result = prices_->reduceChecked(
      stock_locate_, state_page_index, address, side, current_qty,
      canceled_qty);
  if (result != astra::book::MutationResult::Applied)
    return reject(result);
  state->qty = remaining_qty;
  latest_update_timestamp_ = timestamp;
  return astra::book::MutationResult::Applied;
}

astra::book::MutationResult
OrderBook::deleteOrder(std::uint64_t order_id,
                       std::uint64_t timestamp) noexcept {
  if (local_invalid_)
    return astra::book::MutationResult::BookInvalid;
  astra::book::OrderState *const state = orders_->findState(order_id);
  const auto lookup_result = validateState(state);
  if (lookup_result != astra::book::MutationResult::Applied)
    return lookup_result;

  const OrderSide side = decodeSide(state->side);
  const auto level_result =
      prices_->erase(addressOf(*state), side, state->qty);
  if (level_result != astra::book::MutationResult::Applied)
    return reject(level_result);
  const auto table_result = orders_->erase(state);
  if (table_result != astra::book::MutationResult::Applied)
    return reject(astra::book::MutationResult::InvariantViolation);
  --live_order_count_;
  latest_update_timestamp_ = timestamp;
  return astra::book::MutationResult::Applied;
}

astra::book::MutationResult
OrderBook::executeOrder(std::uint64_t order_id,
                        std::uint32_t executed_qty,
                        std::uint64_t timestamp) noexcept {
  if (local_invalid_)
    return astra::book::MutationResult::BookInvalid;
  if (executed_qty == 0)
    return reject(astra::book::MutationResult::InvalidQuantity);
  astra::book::OrderState *const state = orders_->findState(order_id);
  if (state == nullptr)
    return reject(astra::book::MutationResult::OrderNotFound);

  const std::uint16_t state_locate = state->locate;
  const std::uint16_t state_page_index = state->price_page_index;
  const OrderSide side = decodeSide(state->side);
  const std::uint32_t current_qty = state->qty;
  const astra::book::PriceAddress address{
      state->price_page_handle, state->price_level_index, 0};
  if (state_locate != stock_locate_)
    return reject(astra::book::MutationResult::LocateMismatch);
  if (side == static_cast<OrderSide>(0) || current_qty == 0 ||
      !address.valid()) {
    return reject(astra::book::MutationResult::InvariantViolation);
  }
  if (executed_qty > current_qty) {
    if (!prices_->ownsAddress(stock_locate_, state_page_index, address))
      return reject(astra::book::MutationResult::InvariantViolation);
    return reject(astra::book::MutationResult::QuantityExceeded);
  }

  if (executed_qty == current_qty) {
    if (!prices_->ownsAddress(stock_locate_, state_page_index, address))
      return reject(astra::book::MutationResult::InvariantViolation);
    const auto level_result =
        prices_->erase(address, side, current_qty);
    if (level_result != astra::book::MutationResult::Applied)
      return reject(level_result);
    if (orders_->erase(state) != astra::book::MutationResult::Applied)
      return reject(astra::book::MutationResult::InvariantViolation);
    --live_order_count_;
  } else {
    const std::uint32_t remaining_qty = current_qty - executed_qty;
    const auto level_result = prices_->reduceChecked(
        stock_locate_, state_page_index, address, side, current_qty,
        executed_qty);
    if (level_result != astra::book::MutationResult::Applied)
      return reject(level_result);
    state->qty = remaining_qty;
  }
  latest_update_timestamp_ = timestamp;
  return astra::book::MutationResult::Applied;
}

astra::book::MutationResult
OrderBook::replaceOrder(std::uint64_t old_id, std::uint64_t new_id,
                        std::uint64_t new_price,
                        std::uint32_t new_qty,
                        std::uint64_t timestamp) noexcept {
  if (local_invalid_)
    return astra::book::MutationResult::BookInvalid;
  if (new_qty == 0)
    return reject(astra::book::MutationResult::InvalidQuantity);
  if (!astra::protocol::isValidItchPrice4(new_price))
    return reject(astra::book::MutationResult::PriceOutOfRange);

  astra::book::OrderState *const old_state = orders_->findState(old_id);
  const auto old_result = validateState(old_state);
  if (old_result != astra::book::MutationResult::Applied)
    return old_result;

  astra::book::OrderSlotReservation new_order_reservation{};
  if (new_id != old_id) {
    new_order_reservation = orders_->reserve(new_id);
    if (!new_order_reservation.valid())
      return reject(new_order_reservation.result);
  }

  const auto price_reservation = prices_->reservePrice(
      stock_locate_, static_cast<std::uint32_t>(new_price));
  if (!price_reservation.valid())
    return reject(price_reservation.result);
  if (price_reservation.requires_commit) {
    const auto page_commit = prices_->commitReservation(price_reservation);
    if (page_commit != astra::book::MutationResult::Applied)
      return reject(page_commit);
  }

  const OrderSide side = decodeSide(old_state->side);
  const astra::book::PriceAddress old_address = addressOf(*old_state);
  const std::uint32_t old_qty = old_state->qty;

  if (new_id == old_id) {
    const auto move_result = prices_->move(
        old_address, old_qty, price_reservation.address, new_qty, side);
    if (move_result != astra::book::MutationResult::Applied)
      return reject(move_result);
    old_state->qty = new_qty;
    old_state->price_page_handle =
        price_reservation.address.page_handle;
    old_state->price_level_index =
        price_reservation.address.level_index;
    old_state->price_page_index = price_reservation.page_index;
    latest_update_timestamp_ = timestamp;
    return astra::book::MutationResult::Applied;
  }

  astra::book::OrderState new_state{
      new_qty,
      price_reservation.address.page_handle,
      price_reservation.address.level_index,
      stock_locate_,
      static_cast<std::uint8_t>(side),
      0,
      price_reservation.page_index};
  const auto new_commit = orders_->commit(new_order_reservation, new_state);
  if (new_commit != astra::book::MutationResult::Applied)
    return reject(new_commit);

  const auto move_result = prices_->move(
      old_address, old_qty, price_reservation.address, new_qty, side);
  if (move_result != astra::book::MutationResult::Applied) {
    const auto table_rollback = orders_->erase(new_id);
    return reject(table_rollback == astra::book::MutationResult::Applied
                      ? move_result
                      : astra::book::MutationResult::InvariantViolation);
  }

  if (orders_->erase(old_state) != astra::book::MutationResult::Applied) {
    const auto table_rollback = orders_->erase(new_id);
    const auto price_rollback = prices_->move(
        price_reservation.address, new_qty, old_address, old_qty, side);
    if (table_rollback != astra::book::MutationResult::Applied ||
        price_rollback != astra::book::MutationResult::Applied) {
      return reject(astra::book::MutationResult::InvariantViolation);
    }
    return reject(astra::book::MutationResult::InvariantViolation);
  }
  latest_update_timestamp_ = timestamp;
  return astra::book::MutationResult::Applied;
}

TopOfBook OrderBook::getTopOfBook() const noexcept {
  TopOfBook top{};
  top.timestamp = latest_update_timestamp_;
  top.symbol_id = symbol_id_;
  if (prices_ == nullptr)
    return top;

  const auto bid_cursor = prices_->best(stock_locate_, OrderSide::Buy);
  const auto bid = prices_->view(bid_cursor, OrderSide::Buy);
  if (bid.valid) {
    top.bid_price = bid.raw_price;
    top.bid_qty = bid.total_qty;
    top.has_bid = true;
  }

  const auto ask_cursor = prices_->best(stock_locate_, OrderSide::Sell);
  const auto ask = prices_->view(ask_cursor, OrderSide::Sell);
  if (ask.valid) {
    top.ask_price = ask.raw_price;
    top.ask_qty = ask.total_qty;
    top.has_ask = true;
  }
  return top;
}

BookUpdate OrderBook::getBookUpdate() const noexcept {
  BookUpdate update{};
  update.timestamp = latest_update_timestamp_;
  update.symbol_id = symbol_id_;
  if (prices_ == nullptr)
    return update;

  std::array<astra::book::PriceLevelView, 10> bids{};
  std::array<astra::book::PriceLevelView, 10> asks{};
  update.bids_depth = prices_->topTen(stock_locate_, OrderSide::Buy, bids);
  update.asks_depth = prices_->topTen(stock_locate_, OrderSide::Sell, asks);
  for (std::uint8_t i = 0; i < update.bids_depth; ++i) {
    update.bids[i] =
        {bids[i].raw_price, bids[i].total_qty, bids[i].order_count};
  }
  for (std::uint8_t i = 0; i < update.asks_depth; ++i) {
    update.asks[i] =
        {asks[i].raw_price, asks[i].total_qty, asks[i].order_count};
  }
  return update;
}
