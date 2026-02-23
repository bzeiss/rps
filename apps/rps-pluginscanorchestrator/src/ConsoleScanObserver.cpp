#include <rps/orchestrator/ConsoleScanObserver.hpp>
#include <rps/orchestrator/ProcessPool.hpp>
#include <iostream>
#include <boost/filesystem.hpp>

namespace rps::orchestrator {

ConsoleScanObserver::ConsoleScanObserver(bool verbose)
    : m_verbose(verbose) {}

void ConsoleScanObserver::onScanStarted(size_t /*totalPlugins*/, size_t /*workerCount*/) {
    // Header is printed by main.cpp before runJobs
}

void ConsoleScanObserver::onPluginStarted(size_t workerId, size_t /*pluginIndex*/, size_t /*totalPlugins*/,
                                           const std::string& pluginPath) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::cout << "[Worker #" << workerId << "] Scanning: " << pluginPath << "\n";
}

void ConsoleScanObserver::onPluginProgress(size_t /*workerId*/, const std::string& /*pluginPath*/,
                                            int /*percentage*/, const std::string& /*stage*/) {
    // Suppressed in CLI to avoid spam during parallel scan
}

void ConsoleScanObserver::onPluginSlowWarning(size_t workerId, const std::string& pluginPath,
                                               int64_t elapsedMs) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto name = boost::filesystem::path(pluginPath).filename().string();
    std::cerr << "[Worker #" << workerId << " SLOW] " << name
              << " still scanning after " << ProcessPool::formatDuration(elapsedMs) << "...\n";
}

void ConsoleScanObserver::onPluginCompleted(size_t workerId, const std::string& pluginPath,
                                             ScanOutcome outcome, int64_t elapsedMs,
                                             const rps::ipc::ScanResult* result,
                                             const std::string* errorMessage) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto name = boost::filesystem::path(pluginPath).filename().string();
    auto dur = ProcessPool::formatDuration(elapsedMs);

    switch (outcome) {
        case ScanOutcome::Success: {
            std::cout << "[Worker #" << workerId << " SUCCESS] " << name
                      << " -> " << result->name << " v" << result->version
                      << " (" << dur << ")\n";
            if (m_verbose) {
                std::cout << "  Method:   " << (result->scanMethod.empty() ? "unknown" : result->scanMethod) << "\n";
                std::cout << "  Vendor:   " << (result->vendor.empty() ? "(none)" : result->vendor) << "\n";
                std::cout << "  UID:      " << (result->uid.empty() ? "(none)" : result->uid) << "\n";
                std::cout << "  Category: " << (result->category.empty() ? "(none)" : result->category) << "\n";
                std::cout << "  URL:      " << (result->url.empty() ? "(none)" : result->url) << "\n";
                std::cout << "  Desc:     " << (result->description.empty() ? "(none)" : result->description) << "\n";
                std::cout << "  I/O:      " << (result->numInputs || result->numOutputs
                    ? std::to_string(result->numInputs) + " in / " + std::to_string(result->numOutputs) + " out"
                    : "skipped (requires instantiation)") << "\n";
                std::cout << "  Params:   " << (result->parameters.empty()
                    ? "skipped (requires instantiation)"
                    : std::to_string(result->parameters.size()) + " parameter(s)") << "\n";
            }
            break;
        }
        case ScanOutcome::Fail: {
            std::cerr << "[Worker #" << workerId << " FAIL] " << name
                      << " " << (errorMessage ? *errorMessage : "Unknown error")
                      << " (" << dur << ")\n";
            break;
        }
        case ScanOutcome::Crash: {
            std::cerr << "[Worker #" << workerId << " CRASH] " << name
                      << " " << (errorMessage ? *errorMessage : "Unknown crash")
                      << " (" << dur << ")\n";
            break;
        }
        case ScanOutcome::Timeout: {
            std::cerr << "[Worker #" << workerId << " TIMEOUT] " << name
                      << " " << (errorMessage ? *errorMessage : "Process timed out")
                      << " (" << dur << ")\n";
            break;
        }
        case ScanOutcome::Skipped: {
            std::cout << "[Worker #" << workerId << " SKIP] " << name
                      << " " << (errorMessage ? *errorMessage : "Skipped")
                      << "\n";
            break;
        }
    }
}

void ConsoleScanObserver::onWorkerStderrLine(size_t workerId, const std::string& /*pluginPath*/,
                                              const std::string& line) {
    if (m_verbose) {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::cerr << "[Worker #" << workerId << " DBG] " << line << "\n";
    }
}

void ConsoleScanObserver::onWorkerStderrDump(size_t workerId, const std::string& /*pluginPath*/,
                                              const std::vector<std::string>& lines) {
    if (!m_verbose) {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& l : lines) {
            std::cerr << "  [Worker #" << workerId << " stderr] " << l << "\n";
        }
    }
}

void ConsoleScanObserver::onWorkerForceKill(size_t workerId, const std::string& pluginPath) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto name = boost::filesystem::path(pluginPath).filename().string();
    std::cerr << "[Worker #" << workerId << " WARN] " << name
              << " scanner process still alive after result -- force killing.\n";
}

void ConsoleScanObserver::onPluginRetry(size_t workerId, const std::string& pluginPath,
                                         size_t attempt, size_t maxRetries, const std::string& reason) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto name = boost::filesystem::path(pluginPath).filename().string();
    std::cerr << "[Worker #" << workerId << " RETRY] " << name
              << " (attempt " << attempt << "/" << maxRetries << "): " << reason << "\n";
}

void ConsoleScanObserver::onMonitorReport(
    const std::vector<std::pair<size_t, std::pair<std::string, int64_t>>>& activeWorkers) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::cerr << "[Monitor] " << activeWorkers.size() << " worker(s) still active:\n";
    for (auto& [id, info] : activeWorkers) {
        std::cerr << "  Worker #" << id << ": " << info.first
                  << " (" << ProcessPool::formatDuration(info.second) << ")\n";
    }
}

void ConsoleScanObserver::onScanCompleted(size_t success, size_t fail, size_t crash, size_t timeout,
                                           size_t skipped, int64_t totalMs,
                                           const std::vector<std::pair<std::string, std::string>>& failures) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto total = success + fail + crash + timeout + skipped;
    std::cout << "--------------------------------------------------------\n";
    std::cout << "Scan complete: " << total << " plugins | "
              << success << " success, "
              << fail << " failed, "
              << crash << " crashed, "
              << timeout << " timed out, "
              << skipped << " skipped\n";
    std::cout << "Total scan time: " << ProcessPool::formatDuration(totalMs) << "\n";

    if (!failures.empty()) {
        std::cout << "\nFailed plugins (" << failures.size() << "):\n";
        for (auto& [path, reason] : failures) {
            std::cout << "  " << path << "\n";
            std::cout << "    -> " << reason << "\n";
        }
    }
}

} // namespace rps::orchestrator
