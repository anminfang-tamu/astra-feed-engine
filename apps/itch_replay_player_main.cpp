#include "astra/book/BookManager.hpp"
#include "astra/book/TopOfBook.hpp"
#include "astra/codec/BinaryDecoder.hpp"
#include "astra/protocol/MarketDataMessageView.hpp"
#include "replay/SymbolTable.hpp"
#include "replay/itch/ItchParser.hpp"
#include "replay/itch/ItchReplaySource.hpp"

#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>

namespace {

uint64_t parseLimit(const char *value) {
  if (value == nullptr) {
    return std::numeric_limits<uint64_t>::max();
  }
  return std::strtoull(value, nullptr, 10);
}

uint8_t parseChannelId(const char *value) {
  if (value == nullptr) {
    return 0;
  }
  return static_cast<uint8_t>(std::strtoul(value, nullptr, 10));
}

std::string formatPrice(uint64_t price_ticks) {
  std::ostringstream out;
  out << (price_ticks / ItchParser::kPriceScale) << '.' << std::setfill('0')
      << std::setw(4) << (price_ticks % ItchParser::kPriceScale);
  return out.str();
}

} // namespace

int main(int argc, char *argv[]) {
  if (argc < 2) {
    std::cerr << "usage: " << argv[0]
              << " <NASDAQ_ITCH50> [max_messages] [channel_id]\n";
    return EXIT_FAILURE;
  }

  const std::string path = argv[1];
  const uint64_t max_messages = argc >= 3 ? parseLimit(argv[2])
                                          : std::numeric_limits<uint64_t>::max();
  const uint8_t channel_id = argc >= 4 ? parseChannelId(argv[3]) : 0;

  SymbolTable symbols;
  ItchReplaySource source(path, symbols, channel_id);
  if (!source.isOpen()) {
    std::cerr << source.lastError() << "\n";
    return EXIT_FAILURE;
  }

  BinaryDecoder decoder;
  BookManager books;
  uint64_t consumed = 0;

  while (consumed < max_messages) {
    PacketView packet;
    if (!source.next(packet)) {
      break;
    }

    MarketDataMessageView message;
    const DecodeResult result = decoder.decode(packet, message);
    if (result.status != DecodeStatus::Ok) {
      std::cerr << "decode failed after " << consumed << " messages\n";
      return EXIT_FAILURE;
    }

    books.onMessage(message);
    ++consumed;
  }

  const ItchReplayStats &stats = source.stats();
  std::cout << "messages=" << consumed << " records=" << stats.records_read
            << " bytes=" << stats.bytes_read
            << " parsed_events=" << stats.parsed_events
            << " emitted_messages=" << stats.emitted_messages
            << " ignored_events=" << stats.ignored_events
            << " skipped_messages=" << stats.skipped_messages
            << " bad_messages=" << stats.bad_messages
            << " symbols=" << symbols.size() << "\n";

  uint32_t printed = 0;
  for (uint32_t symbol_id = 1; symbol_id <= symbols.size() && printed < 10;
       ++symbol_id) {
    const OrderBook *book = books.getOrderBook(symbol_id);
    const std::string *symbol = symbols.symbol(symbol_id);
    if (book == nullptr || symbol == nullptr) {
      continue;
    }

    const TopOfBook top = book->getTopOfBook();
    std::cout << "symbol=" << *symbol << " id=" << symbol_id
              << " bid=" << formatPrice(top.bid_price) << "x" << top.bid_qty
              << " ask=" << formatPrice(top.ask_price) << "x" << top.ask_qty
              << "\n";
    ++printed;
  }

  if (stats.bad_messages > 0 && !source.lastError().empty()) {
    std::cerr << source.lastError() << "\n";
  }

  return stats.bad_messages == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
