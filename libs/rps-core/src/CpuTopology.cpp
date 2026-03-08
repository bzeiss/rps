#include <rps/core/CpuTopology.hpp>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <format>
#include <set>
#include <thread>

// ---------------------------------------------------------------------------
// Platform-specific includes
// ---------------------------------------------------------------------------

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <avrt.h>  // AvSetMmThreadCharacteristicsW
#pragma comment(lib, "avrt.lib")
#elif defined(__APPLE__)
#include <pthread.h>
#include <sys/sysctl.h>
#elif defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <fstream>
#endif

namespace rps::core {

// ---------------------------------------------------------------------------
// Discovery
// ---------------------------------------------------------------------------

#ifdef _WIN32

CpuTopologyInfo discoverTopology() {
    CpuTopologyInfo topo;

    // Use GetSystemCpuSetInformation to enumerate logical processors
    DWORD bufferLength = 0;
    GetSystemCpuSetInformation(nullptr, 0, &bufferLength, GetCurrentProcess(), 0);

    if (bufferLength == 0) {
        spdlog::warn("CpuTopology: GetSystemCpuSetInformation() returned 0 buffer length, "
                     "falling back to generic topology");
        // Fallback: create generic entries
        uint32_t count = std::thread::hardware_concurrency();
        topo.cores.resize(count);
        for (uint32_t i = 0; i < count; ++i) {
            topo.cores[i].coreId = i;
            topo.cores[i].type = CoreType::Unknown;
        }
        topo.unknownCount = count;
        return topo;
    }

    std::vector<uint8_t> buffer(bufferLength);
    if (!GetSystemCpuSetInformation(
            reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer.data()),
            bufferLength, &bufferLength, GetCurrentProcess(), 0)) {
        spdlog::warn("CpuTopology: GetSystemCpuSetInformation() failed (error {}), "
                     "falling back to generic topology", GetLastError());
        uint32_t count = std::thread::hardware_concurrency();
        topo.cores.resize(count);
        for (uint32_t i = 0; i < count; ++i) {
            topo.cores[i].coreId = i;
            topo.cores[i].type = CoreType::Unknown;
        }
        topo.unknownCount = count;
        return topo;
    }

    // Parse the variable-length entries
    // Track efficiency classes to determine P vs E
    uint8_t minEfficiency = 255;
    uint8_t maxEfficiency = 0;

    struct RawCore {
        uint32_t logicalIndex;
        uint8_t  efficiencyClass;
        uint32_t numaNode;
        uint32_t group;
    };
    std::vector<RawCore> rawCores;

    uint8_t* ptr = buffer.data();
    uint8_t* end = ptr + bufferLength;
    while (ptr < end) {
        auto* info = reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(ptr);
        if (info->Type == CpuSetInformation) {
            RawCore rc;
            rc.logicalIndex = info->CpuSet.LogicalProcessorIndex;
            rc.efficiencyClass = info->CpuSet.EfficiencyClass;
            rc.numaNode = info->CpuSet.NumaNodeIndex;
            rc.group = info->CpuSet.Group;

            if (rc.efficiencyClass < minEfficiency) minEfficiency = rc.efficiencyClass;
            if (rc.efficiencyClass > maxEfficiency) maxEfficiency = rc.efficiencyClass;

            rawCores.push_back(rc);
        }
        ptr += info->Size;
    }

    // Determine if hybrid: multiple distinct efficiency classes
    bool hybrid = (minEfficiency != maxEfficiency) && !rawCores.empty();

    // Build CoreInfo entries
    // On Intel hybrid CPUs, EfficiencyClass 1 = P-core, 0 = E-core
    // (higher EfficiencyClass = higher performance)
    std::set<uint32_t> numaNodes;

    for (const auto& rc : rawCores) {
        CoreInfo ci;
        ci.coreId = rc.logicalIndex;
        ci.numaNode = rc.numaNode;
        ci.packageId = 0; // Not directly available from CpuSet, use group as approximation
        ci.l2CacheId = 0; // Would need CPUID for precise cache topology
        ci.l3CacheId = 0;

        if (hybrid) {
            // Higher efficiency class = P-core
            ci.type = (rc.efficiencyClass == maxEfficiency)
                          ? CoreType::Performance
                          : CoreType::Efficiency;
        } else {
            ci.type = CoreType::Unknown;
        }

        numaNodes.insert(rc.numaNode);
        topo.cores.push_back(ci);
    }

    // Sort by core ID for consistent ordering
    std::sort(topo.cores.begin(), topo.cores.end(),
              [](const CoreInfo& a, const CoreInfo& b) { return a.coreId < b.coreId; });

    // Aggregate counts
    for (const auto& c : topo.cores) {
        switch (c.type) {
            case CoreType::Performance: ++topo.pCoreCount; break;
            case CoreType::Efficiency:  ++topo.eCoreCount; break;
            case CoreType::Unknown:     ++topo.unknownCount; break;
        }
    }
    topo.numaNodeCount = static_cast<uint32_t>(numaNodes.size());
    topo.isHybrid = hybrid;

    return topo;
}

#elif defined(__APPLE__)

CpuTopologyInfo discoverTopology() {
    CpuTopologyInfo topo;
    // macOS: check for performance levels
    int nperflevels = 0;
    size_t len = sizeof(nperflevels);
    if (sysctlbyname("hw.nperflevels", &nperflevels, &len, nullptr, 0) == 0 && nperflevels > 1) {
        topo.isHybrid = true;
        // Get P-core count (perflevel 0 = highest perf)
        int pcores = 0;
        len = sizeof(pcores);
        sysctlbyname("hw.perflevel0.logicalcpu", &pcores, &len, nullptr, 0);
        topo.pCoreCount = static_cast<uint32_t>(pcores);
        topo.eCoreCount = std::thread::hardware_concurrency() - topo.pCoreCount;
    } else {
        topo.unknownCount = std::thread::hardware_concurrency();
    }

    // Create core entries (without specific IDs — macOS doesn't expose per-core mapping)
    uint32_t total = std::thread::hardware_concurrency();
    topo.cores.resize(total);
    for (uint32_t i = 0; i < total; ++i) {
        topo.cores[i].coreId = i;
        // On Apple Silicon, lower IDs tend to be P-cores, but this is not guaranteed
        if (topo.isHybrid) {
            topo.cores[i].type = (i < topo.pCoreCount) ? CoreType::Performance : CoreType::Efficiency;
        } else {
            topo.cores[i].type = CoreType::Unknown;
        }
    }

    return topo;
}

#elif defined(__linux__)

CpuTopologyInfo discoverTopology() {
    CpuTopologyInfo topo;
    uint32_t total = std::thread::hardware_concurrency();
    topo.cores.resize(total);

    // Try to classify by max frequency
    uint64_t maxFreqOverall = 0;
    std::vector<uint64_t> freqs(total, 0);

    for (uint32_t i = 0; i < total; ++i) {
        topo.cores[i].coreId = i;
        std::string path = std::format("/sys/devices/system/cpu/cpu{}/cpufreq/cpuinfo_max_freq", i);
        std::ifstream f(path);
        if (f.is_open()) {
            uint64_t freq = 0;
            f >> freq;
            freqs[i] = freq;
            if (freq > maxFreqOverall) maxFreqOverall = freq;
        }
    }

    // Classify: cores with max_freq >= 90% of highest = P-core
    if (maxFreqOverall > 0) {
        uint64_t threshold = maxFreqOverall * 90 / 100;
        for (uint32_t i = 0; i < total; ++i) {
            if (freqs[i] >= threshold) {
                topo.cores[i].type = CoreType::Performance;
                ++topo.pCoreCount;
            } else if (freqs[i] > 0) {
                topo.cores[i].type = CoreType::Efficiency;
                ++topo.eCoreCount;
            } else {
                topo.cores[i].type = CoreType::Unknown;
                ++topo.unknownCount;
            }
        }
        topo.isHybrid = (topo.pCoreCount > 0 && topo.eCoreCount > 0);
    } else {
        topo.unknownCount = total;
        for (auto& c : topo.cores) c.type = CoreType::Unknown;
    }

    return topo;
}

#else

// Fallback for unsupported platforms
CpuTopologyInfo discoverTopology() {
    CpuTopologyInfo topo;
    uint32_t total = std::thread::hardware_concurrency();
    topo.cores.resize(total);
    for (uint32_t i = 0; i < total; ++i) {
        topo.cores[i].coreId = i;
        topo.cores[i].type = CoreType::Unknown;
    }
    topo.unknownCount = total;
    spdlog::info("CpuTopology: unsupported platform, returning generic topology");
    return topo;
}

#endif

// ---------------------------------------------------------------------------
// Thread affinity
// ---------------------------------------------------------------------------

#ifdef _WIN32

bool pinThreadToCores(const std::vector<uint32_t>& coreIds) {
    if (coreIds.empty()) return false;

    DWORD_PTR mask = 0;
    for (uint32_t id : coreIds) {
        if (id < 64) { // Windows affinity mask is 64-bit
            mask |= (1ULL << id);
        }
    }
    if (mask == 0) return false;

    DWORD_PTR result = SetThreadAffinityMask(GetCurrentThread(), mask);
    if (result == 0) {
        spdlog::warn("CpuTopology: SetThreadAffinityMask() failed (error {})", GetLastError());
        return false;
    }
    return true;
}

#elif defined(__linux__)

bool pinThreadToCores(const std::vector<uint32_t>& coreIds) {
    if (coreIds.empty()) return false;

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    for (uint32_t id : coreIds) {
        CPU_SET(id, &cpuset);
    }

    int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    if (rc != 0) {
        spdlog::warn("CpuTopology: pthread_setaffinity_np() failed (error {})", rc);
        return false;
    }
    return true;
}

#elif defined(__APPLE__)

bool pinThreadToCores(const std::vector<uint32_t>& /*coreIds*/) {
    // macOS does not support direct core pinning.
    // Use QoS classes instead (handled by setRealtimeThreadPriority).
    spdlog::debug("CpuTopology: macOS does not support direct core pinning, using QoS instead");
    return false;
}

#else

bool pinThreadToCores(const std::vector<uint32_t>& /*coreIds*/) {
    spdlog::debug("CpuTopology: pinThreadToCores() not implemented for this platform");
    return false;
}

#endif

bool pinThreadToPerformanceCores(const CpuTopologyInfo& topo) {
    std::vector<uint32_t> pCores;
    for (const auto& c : topo.cores) {
        if (c.type == CoreType::Performance) {
            pCores.push_back(c.coreId);
        }
    }
    if (pCores.empty()) return false;
    return pinThreadToCores(pCores);
}

bool pinThreadToEfficiencyCores(const CpuTopologyInfo& topo) {
    std::vector<uint32_t> eCores;
    for (const auto& c : topo.cores) {
        if (c.type == CoreType::Efficiency) {
            eCores.push_back(c.coreId);
        }
    }
    if (eCores.empty()) return false;
    return pinThreadToCores(eCores);
}

// ---------------------------------------------------------------------------
// Thread priority
// ---------------------------------------------------------------------------

#ifdef _WIN32

bool setRealtimeThreadPriority() {
    // Use MMCSS for audio thread priority
    DWORD taskIndex = 0;
    HANDLE hTask = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    if (hTask == nullptr) {
        spdlog::warn("CpuTopology: AvSetMmThreadCharacteristicsW('Pro Audio') failed "
                     "(error {}), falling back to THREAD_PRIORITY_TIME_CRITICAL",
                     GetLastError());
        // Fallback: use regular thread priority
        if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL)) {
            spdlog::warn("CpuTopology: SetThreadPriority(TIME_CRITICAL) also failed");
            return false;
        }
        return true;
    }
    spdlog::debug("CpuTopology: MMCSS 'Pro Audio' registered (task index {})", taskIndex);
    return true;
}

bool setBackgroundThreadPriority() {
    if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL)) {
        spdlog::warn("CpuTopology: SetThreadPriority(BELOW_NORMAL) failed");
        return false;
    }
    return true;
}

#elif defined(__APPLE__)

bool setRealtimeThreadPriority() {
    // Use QoS class for audio priority
    if (pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0) != 0) {
        spdlog::warn("CpuTopology: pthread_set_qos_class_self_np(USER_INTERACTIVE) failed");
        return false;
    }
    return true;
}

bool setBackgroundThreadPriority() {
    if (pthread_set_qos_class_self_np(QOS_CLASS_BACKGROUND, 0) != 0) {
        spdlog::warn("CpuTopology: pthread_set_qos_class_self_np(BACKGROUND) failed");
        return false;
    }
    return true;
}

#elif defined(__linux__)

bool setRealtimeThreadPriority() {
    struct sched_param param;
    param.sched_priority = 80; // High priority but not maximum
    int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
    if (rc != 0) {
        spdlog::warn("CpuTopology: pthread_setschedparam(SCHED_FIFO, 80) failed "
                     "(error {}) — requires CAP_SYS_NICE", rc);
        // Fallback: try SCHED_RR with lower priority
        param.sched_priority = 50;
        rc = pthread_setschedparam(pthread_self(), SCHED_RR, &param);
        if (rc != 0) {
            spdlog::warn("CpuTopology: SCHED_RR fallback also failed");
            return false;
        }
    }
    return true;
}

bool setBackgroundThreadPriority() {
    struct sched_param param;
    param.sched_priority = 0;
    pthread_setschedparam(pthread_self(), SCHED_OTHER, &param);
    return true;
}

#else

bool setRealtimeThreadPriority() {
    spdlog::debug("CpuTopology: setRealtimeThreadPriority() not implemented");
    return false;
}

bool setBackgroundThreadPriority() {
    spdlog::debug("CpuTopology: setBackgroundThreadPriority() not implemented");
    return false;
}

#endif

// ---------------------------------------------------------------------------
// Summary
// ---------------------------------------------------------------------------

std::string topologySummary(const CpuTopologyInfo& topo) {
    if (topo.isHybrid) {
        return std::format("{}P + {}E cores (hybrid), {} NUMA node{}",
                           topo.pCoreCount, topo.eCoreCount,
                           topo.numaNodeCount,
                           topo.numaNodeCount > 1 ? "s" : "");
    }
    uint32_t total = static_cast<uint32_t>(topo.cores.size());
    return std::format("{} cores (homogeneous), {} NUMA node{}",
                       total, topo.numaNodeCount,
                       topo.numaNodeCount > 1 ? "s" : "");
}

} // namespace rps::core
