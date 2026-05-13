#include "astra/utils/CpuAffinity.hpp"

#include <gtest/gtest.h>

TEST(CpuAffinityTest, AvailableCpuCountIsNonZero) {
  EXPECT_GT(astra::utils::availableCpuCount(), 0u);
}

TEST(CpuAffinityTest, RejectsNegativeCpuId) {
  const auto result = astra::utils::pinCurrentThreadToCpu(-1);

  EXPECT_FALSE(result);
  EXPECT_EQ(result.status, astra::utils::CpuAffinityStatus::InvalidCpu);
}

TEST(CpuAffinityTest, RejectsCpuIdOutsideVisibleRange) {
  const int cpu_id = static_cast<int>(astra::utils::availableCpuCount());
  const auto result = astra::utils::pinCurrentThreadToCpu(cpu_id);

  EXPECT_FALSE(result);
  EXPECT_EQ(result.status, astra::utils::CpuAffinityStatus::InvalidCpu);
}

TEST(CpuAffinityTest, StatusNamesAreStable) {
  EXPECT_STREQ(astra::utils::cpuAffinityStatusName(
                   astra::utils::CpuAffinityStatus::Applied),
               "applied");
  EXPECT_STREQ(astra::utils::cpuAffinityStatusName(
                   astra::utils::CpuAffinityStatus::Unsupported),
               "unsupported");
}
