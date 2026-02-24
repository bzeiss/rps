#pragma once

#include <string>
#include <vector>
#include <utility>
#include <cstdint>
#include <rps/ipc/Messages.hpp>

namespace rps::engine {

enum class ScanOutcome {
    Success,
    Fail,
    Crash,
    Timeout,
    Skipped
};

class ScanObserver {
public:
    virtual ~ScanObserver() = default;

    virtual void onScanStarted(size_t totalPlugins, size_t workerCount) = 0;

    virtual void onPluginStarted(size_t workerId, size_t pluginIndex, size_t totalPlugins,
                                  const std::string& pluginPath) = 0;

    virtual void onPluginProgress(size_t workerId, const std::string& pluginPath,
                                   int percentage, const std::string& stage) = 0;

    virtual void onPluginSlowWarning(size_t workerId, const std::string& pluginPath,
                                      int64_t elapsedMs) = 0;

    virtual void onPluginCompleted(size_t workerId, const std::string& pluginPath,
                                    ScanOutcome outcome, int64_t elapsedMs,
                                    const rps::ipc::ScanResult* result,
                                    const std::string* errorMessage) = 0;

    virtual void onWorkerStderrLine(size_t workerId, const std::string& pluginPath,
                                     const std::string& line) = 0;

    virtual void onWorkerStderrDump(size_t workerId, const std::string& pluginPath,
                                     const std::vector<std::string>& lines) = 0;

    virtual void onWorkerForceKill(size_t workerId, const std::string& pluginPath) = 0;

    virtual void onPluginRetry(size_t workerId, const std::string& pluginPath,
                                size_t attempt, size_t maxRetries, const std::string& reason) = 0;

    virtual void onMonitorReport(const std::vector<std::pair<size_t, std::pair<std::string, int64_t>>>& activeWorkers) = 0;

    virtual void onScanCompleted(size_t success, size_t fail, size_t crash, size_t timeout,
                                  size_t skipped, int64_t totalMs,
                                  const std::vector<std::pair<std::string, std::string>>& failures) = 0;
};

} // namespace rps::engine
