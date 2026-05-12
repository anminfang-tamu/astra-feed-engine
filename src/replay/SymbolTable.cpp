#include "replay/SymbolTable.hpp"

uint32_t SymbolTable::intern(std::string_view symbol) {
  const auto existing = id_by_symbol_.find(std::string(symbol));
  if (existing != id_by_symbol_.end()) {
    return existing->second;
  }

  symbols_.emplace_back(symbol);
  const uint32_t id = static_cast<uint32_t>(symbols_.size());
  id_by_symbol_.emplace(symbols_.back(), id);
  return id;
}

uint32_t SymbolTable::find(std::string_view symbol) const {
  const auto it = id_by_symbol_.find(std::string(symbol));
  return it == id_by_symbol_.end() ? 0 : it->second;
}

const std::string *SymbolTable::symbol(uint32_t symbol_id) const {
  if (symbol_id == 0 || symbol_id > symbols_.size()) {
    return nullptr;
  }
  return &symbols_[symbol_id - 1];
}

std::size_t SymbolTable::size() const { return symbols_.size(); }
