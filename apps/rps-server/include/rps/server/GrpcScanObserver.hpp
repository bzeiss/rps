#pragma once

#include <rps/engine/ScanObserver.hpp>
#include <grpcpp/grpcpp.h>
#include <mutex>

// Forward declare generated types
namespace rps { namespace v1 { class ScanEvent; } }

namespace rps::server {

class GrpcScanObserver : public rps::engine::ScanObserver {
public:
    explicit GrpcScanObserver(grpc::ServerWriter<rps::v1::ScanEvent>* writer);

    void onScanStarted(size_t totalPlugins, size_t workerCount) override;

    void onPluginStarted(size_t workerId, size_t pluginIndex, size_t totalPlugins,
                          const std::string& pluginPath) override;

    void onPluginProgress(size_t workerId, const std::string& pluginPath,
                           int percentage, const std::string& stage) override;

    void onPluginSlowWarning(size_t workerId, const std::string& pluginPath,
                              int64_t elapsedMs) override;

    void onPluginCompleted(size_t workerId, const std::string& pluginPath,
                            rps::engine::ScanOutcome outcome, int64_t elapsedMs,
                            const rps::ipc::ScanResult* result,
                            const std::string* errorMessage) override;

    void onWorkerStderrLine(size_t workerId, const std::string& pluginPath,
                             const std::string& line) override;

    void onWorkerStderrDump(size_t workerId, const std::string& pluginPath,
                             const std::vector<std::string>& lines) override;

    void onWorkerStdoutLine(size_t workerId, const std::string& pluginPath,
                             const std::string& line) override;

    void onWorkerStdoutDump(size_t workerId, const std::string& pluginPath,
                             const std::vector<std::string>& lines) override;

    void onWorkerForceKill(size_t workerId, const std::string& pluginPath) override;

    void onPluginRetry(size_t workerId, const std::string& pluginPath,
                        size_t attempt, size_t maxRetries, const std::string& reason) override;

    void onMonitorReport(const std::vector<std::pair<size_t, std::pair<std::string, int64_t>>>& activeWorkers) override;

    void onScanCompleted(size_t success, size_t fail, size_t crash, size_t timeout,
                          size_t skipped, int64_t totalMs,
                          const std::vector<std::pair<std::string, std::string>>& failures) override;

    // Set incremental scan stats (called before runScan to populate ScanStarted)
    void setIncrementalStats(uint32_t skippedUnchanged, uint32_t skippedBlocked);

private:
    grpc::ServerWriter<rps::v1::ScanEvent>* m_writer;
    std::mutex m_mutex;
    uint32_t m_skippedUnchanged = 0;
    uint32_t m_skippedBlocked = 0;
};

} // namespace rps::server
