#include "replay/taq/NyseTaqParser.hpp"

#include <charconv>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

namespace {

std::string_view trim(std::string_view value) {
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.front())) != 0) {
    value.remove_prefix(1);
  }
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.back())) != 0) {
    value.remove_suffix(1);
  }
  if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
    value.remove_prefix(1);
    value.remove_suffix(1);
  }
  return value;
}

void splitCsv(std::string_view line, std::vector<std::string_view> &fields) {
  fields.clear();
  bool in_quotes = false;
  std::size_t field_start = 0;

  for (std::size_t i = 0; i < line.size(); ++i) {
    const char ch = line[i];
    if (ch == '"') {
      in_quotes = !in_quotes;
    } else if (ch == ',' && !in_quotes) {
      fields.push_back(trim(line.substr(field_start, i - field_start)));
      field_start = i + 1;
    }
  }

  fields.push_back(trim(line.substr(field_start)));
}

template <typename T> bool parseUnsigned(std::string_view value, T &out) {
  value = trim(value);
  if (value.empty()) {
    return false;
  }

  T parsed{};
  const char *begin = value.data();
  const char *end = value.data() + value.size();
  const auto result = std::from_chars(begin, end, parsed);
  if (result.ec != std::errc{} || result.ptr != end) {
    return false;
  }
  out = parsed;
  return true;
}

bool parseSide(std::string_view value, char &side) {
  value = trim(value);
  if (value.empty()) {
    side = '\0';
    return true;
  }

  const char ch = static_cast<char>(
      std::toupper(static_cast<unsigned char>(value.front())));
  if (ch == 'B' || ch == '1') {
    side = 'B';
    return true;
  }
  if (ch == 'S' || ch == '2') {
    side = 'S';
    return true;
  }
  return false;
}

bool mapMessageType(uint32_t raw_type, MdEventType &type) {
  switch (raw_type) {
  case 100:
  case 106:
    type = MdEventType::Add;
    return true;
  case 101:
    type = MdEventType::Modify;
    return true;
  case 102:
    type = MdEventType::Delete;
    return true;
  case 103:
    type = MdEventType::Execution;
    return true;
  case 104:
    type = MdEventType::Replace;
    return true;
  case 105:
    type = MdEventType::Imbalance;
    return true;
  case 34:
    type = MdEventType::Status;
    return true;
  case 223:
    type = MdEventType::Summary;
    return true;
  case 110:
  case 111:
  case 112:
  case 220:
  case 221:
  case 222:
    type = MdEventType::Trade;
    return true;
  default:
    type = MdEventType::Unknown;
    return false;
  }
}

bool isHeaderOrComment(std::string_view first_field) {
  first_field = trim(first_field);
  if (first_field.empty() || first_field.front() == '#') {
    return true;
  }

  uint32_t ignored = 0;
  return !parseUnsigned(first_field, ignored);
}

bool requireField(const std::vector<std::string_view> &fields,
                  std::size_t index, std::string &error) {
  if (index < fields.size()) {
    return true;
  }
  error = "line has too few fields";
  return false;
}

} // namespace

NyseTaqParser::NyseTaqParser(SymbolTable &symbols, uint8_t channel_id)
    : symbols_(symbols), channel_id_(channel_id) {}

bool NyseTaqParser::parseLine(std::string_view line, MdEvent &event) {
  last_error_.clear();
  last_line_skipped_ = false;

  std::vector<std::string_view> fields;
  splitCsv(line, fields);
  if (fields.empty() || isHeaderOrComment(fields[0])) {
    last_line_skipped_ = true;
    return false;
  }

  uint32_t raw_type = 0;
  if (!parseUnsigned(fields[0], raw_type)) {
    last_error_ = "invalid message type";
    return false;
  }

  event = MdEvent{};
  event.channel_id = channel_id_;
  if (!mapMessageType(raw_type, event.type)) {
    last_line_skipped_ = true;
    return false;
  }

  std::size_t symbol_index = 3;
  std::size_t symbol_seq_index = 4;
  std::size_t order_id_index = std::numeric_limits<std::size_t>::max();
  std::size_t new_order_id_index = std::numeric_limits<std::size_t>::max();
  std::size_t price_index = std::numeric_limits<std::size_t>::max();
  std::size_t qty_index = std::numeric_limits<std::size_t>::max();
  std::size_t side_index = std::numeric_limits<std::size_t>::max();

  switch (raw_type) {
  case 100:
    order_id_index = 5;
    price_index = 6;
    qty_index = 7;
    side_index = 8;
    break;
  case 101:
    order_id_index = 5;
    price_index = 6;
    qty_index = 7;
    side_index = 9;
    break;
  case 102:
    order_id_index = 5;
    break;
  case 103:
    order_id_index = 5;
    price_index = 7;
    qty_index = 8;
    break;
  case 104:
    order_id_index = 5;
    new_order_id_index = 6;
    price_index = 7;
    qty_index = 8;
    side_index = 9;
    break;
  case 105:
    symbol_index = 4;
    symbol_seq_index = 5;
    break;
  case 106:
    symbol_index = 4;
    symbol_seq_index = 5;
    order_id_index = 6;
    price_index = 7;
    qty_index = 8;
    side_index = 9;
    break;
  case 110:
  case 111:
  case 220:
    order_id_index = 5;
    price_index = 6;
    qty_index = 7;
    break;
  case 222:
    symbol_index = 4;
    symbol_seq_index = 5;
    order_id_index = 7;
    price_index = 8;
    qty_index = 9;
    break;
  default:
    break;
  }

  if (!requireField(fields, 2, last_error_) ||
      !requireField(fields, symbol_index, last_error_)) {
    return false;
  }

  if (!parseUnsigned(fields[1], event.channel_seq_no)) {
    last_error_ = "invalid sequence number";
    return false;
  }
  if (!parseSourceTimeNs(fields[2], event.ts_event_ns)) {
    last_error_ = "invalid source time";
    return false;
  }

  const std::string_view symbol = fields[symbol_index];
  if (!symbol.empty()) {
    event.symbol_id = symbols_.intern(symbol);
  }

  if (symbol_seq_index < fields.size() && !fields[symbol_seq_index].empty()) {
    if (!parseUnsigned(fields[symbol_seq_index], event.symbol_seq_no)) {
      last_error_ = "invalid symbol sequence number";
      return false;
    }
  }

  if (order_id_index != std::numeric_limits<std::size_t>::max()) {
    if (!requireField(fields, order_id_index, last_error_) ||
        !parseUnsigned(fields[order_id_index], event.order_id)) {
      last_error_ = "invalid order id";
      return false;
    }
  }

  if (new_order_id_index != std::numeric_limits<std::size_t>::max()) {
    if (!requireField(fields, new_order_id_index, last_error_) ||
        !parseUnsigned(fields[new_order_id_index], event.new_order_id)) {
      last_error_ = "invalid new order id";
      return false;
    }
  }

  if (price_index != std::numeric_limits<std::size_t>::max()) {
    if (!requireField(fields, price_index, last_error_) ||
        !parsePriceTicks(fields[price_index], event.price_ticks)) {
      last_error_ = "invalid price";
      return false;
    }
  }

  if (qty_index != std::numeric_limits<std::size_t>::max()) {
    if (!requireField(fields, qty_index, last_error_) ||
        !parseUnsigned(fields[qty_index], event.qty)) {
      last_error_ = "invalid quantity";
      return false;
    }
  }

  if (side_index != std::numeric_limits<std::size_t>::max()) {
    if (!requireField(fields, side_index, last_error_) ||
        !parseSide(fields[side_index], event.side)) {
      last_error_ = "invalid side";
      return false;
    }
  }

  return true;
}

void NyseTaqParser::setChannelId(uint8_t channel_id) {
  channel_id_ = channel_id;
}

uint8_t NyseTaqParser::channelId() const { return channel_id_; }

bool NyseTaqParser::lastLineSkipped() const { return last_line_skipped_; }

const std::string &NyseTaqParser::lastError() const { return last_error_; }

bool NyseTaqParser::parsePriceTicks(std::string_view value,
                                    int64_t &price_ticks) {
  value = trim(value);
  if (value.empty()) {
    return false;
  }

  bool negative = false;
  if (value.front() == '+' || value.front() == '-') {
    negative = value.front() == '-';
    value.remove_prefix(1);
  }

  std::size_t dot = value.find('.');
  const std::string_view whole_part =
      dot == std::string_view::npos ? value : value.substr(0, dot);

  int64_t whole = 0;
  bool has_digit = false;
  if (!whole_part.empty()) {
    if (!parseUnsigned(whole_part, whole)) {
      return false;
    }
    has_digit = true;
  }

  if (whole_part.empty() && dot == std::string_view::npos) {
    return false;
  }

  int64_t frac = 0;
  int64_t scale = kPriceScale / 10;
  if (dot != std::string_view::npos) {
    const std::string_view frac_part = value.substr(dot + 1);
    for (const char ch : frac_part) {
      if (!std::isdigit(static_cast<unsigned char>(ch))) {
        return false;
      }
      has_digit = true;
      if (scale > 0) {
        frac += static_cast<int64_t>(ch - '0') * scale;
        scale /= 10;
      }
    }
  }

  if (!has_digit) {
    return false;
  }

  price_ticks = whole * kPriceScale + frac;
  if (negative) {
    price_ticks = -price_ticks;
  }
  return true;
}

bool NyseTaqParser::parseSourceTimeNs(std::string_view value, uint64_t &ns) {
  value = trim(value);
  if (value.empty()) {
    return false;
  }

  const std::size_t first_colon = value.find(':');
  const std::size_t second_colon =
      first_colon == std::string_view::npos
          ? std::string_view::npos
          : value.find(':', first_colon + 1);
  if (first_colon == std::string_view::npos ||
      second_colon == std::string_view::npos) {
    return false;
  }

  uint32_t hours = 0;
  uint32_t minutes = 0;
  uint32_t seconds = 0;
  if (!parseUnsigned(value.substr(0, first_colon), hours) ||
      !parseUnsigned(value.substr(first_colon + 1,
                                  second_colon - first_colon - 1),
                     minutes)) {
    return false;
  }

  const std::string_view second_part = value.substr(second_colon + 1);
  const std::size_t dot = second_part.find('.');
  const std::string_view whole_seconds =
      dot == std::string_view::npos ? second_part : second_part.substr(0, dot);
  if (!parseUnsigned(whole_seconds, seconds)) {
    return false;
  }

  if (hours > 23 || minutes > 59 || seconds > 60) {
    return false;
  }

  uint64_t frac_ns = 0;
  uint64_t scale = 100000000;
  if (dot != std::string_view::npos) {
    const std::string_view frac = second_part.substr(dot + 1);
    for (const char ch : frac) {
      if (!std::isdigit(static_cast<unsigned char>(ch))) {
        return false;
      }
      if (scale > 0) {
        frac_ns += static_cast<uint64_t>(ch - '0') * scale;
        scale /= 10;
      }
    }
  }

  const uint64_t seconds_since_midnight =
      (static_cast<uint64_t>(hours) * 3600) +
      (static_cast<uint64_t>(minutes) * 60) + seconds;
  ns = seconds_since_midnight * 1000000000ULL + frac_ns;
  return true;
}
