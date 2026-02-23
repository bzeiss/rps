#pragma once

#include <rps/orchestrator/ScanObserver.hpp>
#include <mutex>

namespace rps::orchestrator {

class ConsoleScanObserver : public ScanObserver {
public:
    explicit ConsoleScanObserver(bool verbose);

    void onScanStarted(size_t totalPlugins, size_t workerCount) override;

    void onPluginStarted(size_t workerId, size_t pluginIndex, size_t totalPlugins,
                          const std::string& pluginPath) override;

    void onPluginProgress(size_t workerId, const std::string& pluginPath,
                           int percentage, const std::string& stage) override;

    void onPluginSlowWarning(size_t workerId, const std::string& pluginPath,
                              int64_t elapsedMs) override;

    void onPluginCompleted(size_t workerId, const std::string& pluginPath,
                            ScanOutcome outcome, int64_t elapsedMs,
                            const rps::ipc::ScanResult* result,
                            const std::string* errorMessage) override;

    void onWorkerStderrLine(size_t workerId, const std::string& pluginPath,
                             const std::string& line) override;

    void onWorkerStderrDump(size_t workerId, const std::string& pluginPath,
                             const std::vector<std::string>& lines) override;

    void onWorkerForceKill(size_t workerId, const std::string& pluginPath) override;

    void onPluginRetry(size_t workerId, const std::string& pluginPath,
                        size_t attempt, size_t maxRetries, const std::string& reason) override;

    void onMonitorReport(const std::vector<std::pair<size_t, std::pair<std::string, int64_t>>>& activeWorkers) override;

    void onScanCompleted(size_t success, size_t fail, size_t crash, size_t timeout,
                          int64_t totalMs,
                          const std::vector<std::pair<std::string, std::string>>& failures) override;

private:
    bool m_verbose;
    std::mutex m_mutex;
};

} // namespace rps::orchestrator
