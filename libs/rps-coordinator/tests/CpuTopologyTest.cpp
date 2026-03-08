#include "TestFramework.hpp"
#include <rps/core/CpuTopology.hpp>

#include <set>
#include <thread>

using namespace rps::core;

TEST(CpuTopology_DiscoverReturnsNonEmpty) {
    auto topo = discoverTopology();
    ASSERT_TRUE(!topo.cores.empty());
    ASSERT_EQ(topo.cores.size(), static_cast<size_t>(std::thread::hardware_concurrency()));
}

TEST(CpuTopology_CoreCountsMatchTotal) {
    auto topo = discoverTopology();
    uint32_t sum = topo.pCoreCount + topo.eCoreCount + topo.unknownCount;
    ASSERT_EQ(sum, static_cast<uint32_t>(topo.cores.size()));
}

TEST(CpuTopology_HybridConsistency) {
    auto topo = discoverTopology();
    if (topo.isHybrid) {
        // Hybrid systems must have both P and E cores
        ASSERT_TRUE(topo.pCoreCount > 0);
        ASSERT_TRUE(topo.eCoreCount > 0);
    } else {
        // Non-hybrid: either all unknown, or all one type
        ASSERT_TRUE(topo.pCoreCount == 0 || topo.eCoreCount == 0);
    }
}

TEST(CpuTopology_CoreIdsAreUnique) {
    auto topo = discoverTopology();
    std::set<uint32_t> ids;
    for (const auto& c : topo.cores) {
        ids.insert(c.coreId);
    }
    ASSERT_EQ(ids.size(), topo.cores.size());
}

TEST(CpuTopology_AtLeastOneNumaNode) {
    auto topo = discoverTopology();
    ASSERT_TRUE(topo.numaNodeCount >= 1);
}

TEST(CpuTopology_PinToCoresDoesNotCrash) {
    auto topo = discoverTopology();
    if (topo.cores.empty()) return;
    // Pin to first core — should succeed on Windows/Linux, no-op on macOS
    pinThreadToCores({topo.cores[0].coreId});
    // Reset: pin to all cores
    std::vector<uint32_t> allCores;
    for (const auto& c : topo.cores) allCores.push_back(c.coreId);
    pinThreadToCores(allCores);
}

TEST(CpuTopology_SummaryNonEmpty) {
    auto topo = discoverTopology();
    auto summary = topologySummary(topo);
    ASSERT_TRUE(!summary.empty());
}
