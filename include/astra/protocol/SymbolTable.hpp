#pragma once

#include "astra/protocol/SymbolInfo.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

// Fixed-size symbol table indexed directly by ITCH Stock Locate (uint16, 1–8191).
// O(1) access with no dynamic allocation; stock locate 0 is reserved/invalid.
class SymbolTable {
public:
  static constexpr uint16_t kMaxLocate = 16384;

  // Populate from a parsed Stock Directory ('R') message.
  void set(uint16_t locate, const SymbolInfo &info) noexcept {
    if (locate == 0 || locate >= kMaxLocate) return;
    if (symbols_[locate].ticker[0] == '\0') ++count_;
    symbols_[locate] = info;
  }

  // Lightweight fallback called from Add Order ('A'/'F') when Stock Directory
  // hasn't been seen yet for this locate. Does nothing if already registered.
  void setTicker(uint16_t locate, std::string_view sym) noexcept {
    if (locate == 0 || locate >= kMaxLocate) return;
    if (symbols_[locate].ticker[0] != '\0') return;
    const std::size_t n = sym.size() < 8 ? sym.size() : 8;
    std::memcpy(symbols_[locate].ticker, sym.data(), n);
    symbols_[locate].ticker[n] = '\0';
    symbols_[locate].locate    = locate;
    ++count_;
  }

  const SymbolInfo *get(uint16_t locate) const noexcept {
    if (locate == 0 || locate >= kMaxLocate || symbols_[locate].ticker[0] == '\0')
      return nullptr;
    return &symbols_[locate];
  }

  const char *ticker(uint16_t locate) const noexcept {
    if (locate == 0 || locate >= kMaxLocate || symbols_[locate].ticker[0] == '\0')
      return nullptr;
    return symbols_[locate].ticker;
  }

  bool isRegistered(uint16_t locate) const noexcept {
    return locate > 0 && locate < kMaxLocate && symbols_[locate].ticker[0] != '\0';
  }

  std::size_t size() const noexcept { return count_; }

private:
  // Zero-initialized; ticker[0] == '\0' indicates an unregistered locate slot.
  SymbolInfo  symbols_[kMaxLocate]{};
  std::size_t count_{0};
};
