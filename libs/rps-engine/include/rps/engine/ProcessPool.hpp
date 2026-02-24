#pragma once

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include <boost/filesystem.hpp>
#include <rps/engine/db/DatabaseManager.hpp>
#include <rps/engine/ScanObserver.hpp>

namespace rps::engine {

struct ScanJob {
    boost::filesystem::path pluginPath;
    std::string scannerBin;
    int timeoutMs;
    bool verbose = false;
    size_t pluginIndex = 0;
    size_t totalPlugins = 0;
    size_t maxRetries = 3;
    size_t attempt = 0;  // 0 = first try, 1 = first retry, etc.
    std::string format;  // e.g. "vst3", "clap", "aax" — used to persist skips
};

class ProcessPool {
public:
    ProcessPool(size_t maxWorkers, db::DatabaseManager* db = nullptr,
                ScanObserver* observer = nullptr);
    ~ProcessPool();

    void runJobs(const std::vector<ScanJob>& jobs);

    static std::string formatDuration(int64_t ms);

    struct ScanStats {
        size_t success = 0;
        size_t fail = 0;
        size_t crash = 0;
        size_t timeout = 0;
        size_t skipped = 0;
        size_t total() const { return success + fail + crash + timeout + skipped; }
    };
    ScanStats stats() const;
    std::vector<std::pair<std::string, std::string>> failures() const;

private:
    void workerThreadLoop(size_t workerId);
    void processJob(const ScanJob& job, size_t workerId);

    size_t m_maxWorkers;
    db::DatabaseManager* m_db = nullptr;
    ScanObserver* m_observer = nullptr;
    std::vector<std::thread> m_threads;
    std::vector<ScanJob> m_jobQueue;
    std::mutex m_queueMutex;
    std::atomic<bool> m_stop{false};
    std::atomic<size_t> m_success{0};
    std::atomic<size_t> m_fail{0};
    std::atomic<size_t> m_crash{0};
    std::atomic<size_t> m_timeout{0};
    std::atomic<size_t> m_skipped{0};

    struct WorkerInfo {
        std::string pluginPath;
        std::chrono::steady_clock::time_point startTime;
    };
    std::mutex m_activeMutex;
    std::map<size_t, WorkerInfo> m_activeWorkers;

    // Retry queue: failed jobs get re-enqueued with incremented attempt
    std::vector<ScanJob> m_retryQueue;
    std::mutex m_retryMutex;

    // Final failure list: pluginPath -> last error reason
    std::vector<std::pair<std::string, std::string>> m_failures;
    std::mutex m_failureMutex;

    void recordFailure(const std::string& pluginPath, const std::string& reason);
    bool enqueueRetry(const ScanJob& job, const std::string& reason, size_t workerId);
};

} // namespace rps::engine
