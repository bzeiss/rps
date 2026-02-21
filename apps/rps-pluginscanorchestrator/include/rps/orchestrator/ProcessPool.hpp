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
};

class ProcessPool {
public:
    ProcessPool(size_t maxWorkers, db::DatabaseManager* db = nullptr);
    ~ProcessPool();

    // Enqueue jobs and block until all are finished.
    void runJobs(const std::vector<ScanJob>& jobs);

private:
    void workerThreadLoop();
    void processJob(const ScanJob& job);

    size_t m_maxWorkers;
    db::DatabaseManager* m_db = nullptr;
    std::vector<std::thread> m_threads;
    std::vector<ScanJob> m_jobQueue;
    std::mutex m_queueMutex;
    std::atomic<bool> m_stop{false};
};

} // namespace rps::orchestrator
