#pragma once

#include "astra/book/BookManager.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace astra::benchmark_fixture {

inline constexpr std::string_view kNasdaqItch20190130Name =
    "nasdaq-itch-20190130-acceptance-v1";
inline constexpr std::string_view kNasdaqItch20190130TraceSha256 =
    "1d0972ffc25b35902ccc3f9069aae517da56903d5795f872902b8697315f30c3";
inline constexpr std::string_view kNasdaqItch20190130ProfilerSha256 =
    "c7f468bd3bc784398626997329f01653ac54b8691af822419931f23e95907956";

inline constexpr BookManagerConfig
nasdaqItch20190130AcceptanceConfig() noexcept {
  return {{std::uint64_t{1} << 29, std::uint32_t{1} << 20, true},
          {80'000, true}};
}

inline BookCapacityProfile nasdaqItch20190130AcceptanceProfile() {
  constexpr BookManagerConfig config =
      nasdaqItch20190130AcceptanceConfig();
  constexpr BookCapacityEvidence evidence{329'176'641, 68'941};
  return {
      std::string{kNasdaqItch20190130Name},
      std::string{kNasdaqItch20190130TraceSha256},
      config,
      evidence,
      {config.orders.direct_slots -
           (evidence.profiled_max_order_reference + 1),
       config.prices.page_capacity -
           evidence.profiled_unique_price_pages},
  };
}

} // namespace astra::benchmark_fixture
