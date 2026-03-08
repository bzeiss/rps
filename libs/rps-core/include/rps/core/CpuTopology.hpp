#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rps::core {

// ---------------------------------------------------------------------------
// CPU Core Type classification
// ---------------------------------------------------------------------------

enum class CoreType : uint8_t {
    Performance,   ///< High-clock P-core (Intel) / Performance cluster (Apple M-series)
    Efficiency,    ///< E-core (Intel) / Efficiency cluster (Apple M-series)
    Unknown        ///< Classification unavailable (homogeneous CPU or unsupported platform)
};

// ---------------------------------------------------------------------------
// Per-core information
// ---------------------------------------------------------------------------

struct CoreInfo {
    uint32_t coreId   = 0;        ///< Logical processor index (OS core ID)
    CoreType type     = CoreType::Unknown;
    uint32_t numaNode = 0;        ///< NUMA node index
    uint32_t packageId = 0;       ///< Physical CPU package
    uint32_t l2CacheId = 0;       ///< Shared L2 cache group (cores sharing L2)
    uint32_t l3CacheId = 0;       ///< Shared L3 cache group
};

// ---------------------------------------------------------------------------
// Aggregate topology info
// ---------------------------------------------------------------------------

struct CpuTopologyInfo {
    std::vector<CoreInfo> cores;
    uint32_t pCoreCount     = 0;
    uint32_t eCoreCount     = 0;
    uint32_t unknownCount   = 0;
    uint32_t numaNodeCount  = 1;
    bool     isHybrid       = false;  ///< true if P+E mix detected
};

// ---------------------------------------------------------------------------
// Discovery
// ---------------------------------------------------------------------------

/// Query the current system's CPU topology.
/// Returns a populated CpuTopologyInfo. On unsupported platforms or if
/// discovery fails, returns a generic topology with all cores marked Unknown
/// and isHybrid = false.
CpuTopologyInfo discoverTopology();

// ---------------------------------------------------------------------------
// Thread affinity
// ---------------------------------------------------------------------------

/// Pin the calling thread to a specific set of cores.
/// Returns true if at least one core was successfully set.
bool pinThreadToCores(const std::vector<uint32_t>& coreIds);

/// Pin the calling thread to all P-cores (convenience).
/// If no P-cores are known, does nothing and returns false.
bool pinThreadToPerformanceCores(const CpuTopologyInfo& topo);

/// Pin the calling thread to all E-cores (convenience).
/// If no E-cores are known, does nothing and returns false.
bool pinThreadToEfficiencyCores(const CpuTopologyInfo& topo);

// ---------------------------------------------------------------------------
// Thread priority
// ---------------------------------------------------------------------------

/// Elevate the calling thread to real-time / audio priority.
/// - Windows: AvSetMmThreadCharacteristicsW("Pro Audio")
/// - macOS:   QOS_CLASS_USER_INTERACTIVE
/// - Linux:   SCHED_FIFO with elevated priority
/// Returns true on success, false on failure (logged as warning).
bool setRealtimeThreadPriority();

/// Lower the calling thread's priority for background work.
/// Returns true on success.
bool setBackgroundThreadPriority();

/// Returns a human-readable summary string for logging.
/// Example: "8P + 8E cores (hybrid), 1 NUMA node"
std::string topologySummary(const CpuTopologyInfo& topo);

} // namespace rps::core
