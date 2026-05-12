#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class SymbolTable {
public:
  uint32_t intern(std::string_view symbol);
  uint32_t find(std::string_view symbol) const;
  const std::string *symbol(uint32_t symbol_id) const;
  std::size_t size() const;

private:
  std::vector<std::string> symbols_;
  std::unordered_map<std::string, uint32_t> id_by_symbol_;
};
