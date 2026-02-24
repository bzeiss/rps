#pragma once

#include <string>
#include <vector>
#include <rps/engine/ProcessPool.hpp>
#include <rps/engine/ScanObserver.hpp>

namespace rps::engine {

struct ScanConfig {
    std::vector<std::string> scanDirs;       // --scan-dir
    std::string singlePlugin;                // --scan (empty = not set)
#ifdef _WIN32
    std::string scannerBin = "rps-pluginscanner.exe";
#else
    std::string scannerBin = "rps-pluginscanner";
#endif
    std::string dbPath = "rps-plugins.db";
    std::string mode = "incremental";        // "full" | "incremental"
    std::string formats = "all";             // comma-separated or "all"
    std::string filter;                      // filename substring filter
    size_t limit = 0;                        // 0 = unlimited
    size_t jobs = 6;
    size_t retries = 3;
    int timeoutMs = 120000;
    bool verbose = false;
};

struct ScanSummary {
    size_t success = 0;
    size_t fail = 0;
    size_t crash = 0;
    size_t timeout = 0;
    size_t skipped = 0;
    size_t skippedUnchanged = 0;  // incremental mode: unchanged plugins
    size_t skippedBlocked = 0;    // incremental mode: previously blocked
    int64_t totalMs = 0;
    std::vector<std::pair<std::string, std::string>> failures;  // path -> reason
};

class ScanEngine {
public:
    ScanEngine();

    // Run a scan to completion. Observer receives all progress events.
    // Thread-safe: only one scan at a time (returns empty summary if busy).
    ScanSummary runScan(const ScanConfig& config, ScanObserver* observer);

    // Query available format names
    std::vector<std::string> availableFormats() const;

    // Check if a scan is currently running
    bool isScanning() const;

private:
    std::atomic<bool> m_scanning{false};
};

} // namespace rps::engine
