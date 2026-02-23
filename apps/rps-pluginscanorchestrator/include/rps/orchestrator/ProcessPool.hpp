#pragma once

#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <thread>
#include <atomic>
#include <boost/filesystem.hpp>
#include <boost/process/v1.hpp>
#include <rps/ipc/Connection.hpp>
#include <rps/orchestrator/db/DatabaseManager.hpp>

namespace rps::orchestrator {

struct ScanJob {
    boost::filesystem::path pluginPath;
    std::string scannerBin;
    int timeoutMs;
    bool verbose = false;
};

class ProcessPool {
public:
    ProcessPool(size_t maxWorkers, db::DatabaseManager* db = nullptr);
    ~ProcessPool();

    // Enqueue jobs and block until all are finished.
    void runJobs(const std::vector<ScanJob>& jobs);

    static std::string formatDuration(int64_t ms);

    struct ScanStats {
        size_t success = 0;
        size_t fail = 0;
        size_t crash = 0;
        size_t timeout = 0;
        size_t total() const { return success + fail + crash + timeout; }
    };
    ScanStats stats() const;

private:
    void workerThreadLoop(size_t workerId);
    void processJob(const ScanJob& job, size_t workerId);

    size_t m_maxWorkers;
    db::DatabaseManager* m_db = nullptr;
    std::vector<std::thread> m_threads;
    std::vector<ScanJob> m_jobQueue;
    std::mutex m_queueMutex;
    std::mutex m_consoleMutex;
    std::atomic<bool> m_stop{false};
    std::atomic<size_t> m_success{0};
    std::atomic<size_t> m_fail{0};
    std::atomic<size_t> m_crash{0};
    std::atomic<size_t> m_timeout{0};
};

} // namespace rps::orchestrator
