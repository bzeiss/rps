#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif

#include <rps/orchestrator/ProcessPool.hpp>
#include <iostream>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

namespace rps::orchestrator {

namespace bp = boost::process::v1;

ProcessPool::ProcessPool(size_t maxWorkers, db::DatabaseManager* db)
    : m_maxWorkers(maxWorkers), m_db(db) {}

ProcessPool::~ProcessPool() {
    m_stop = true;
    for (auto& t : m_threads) {
        if (t.joinable()) t.join();
    }
}

void ProcessPool::runJobs(const std::vector<ScanJob>& jobs) {
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_jobQueue = jobs;
    }

    size_t numThreads = std::min(m_maxWorkers, jobs.size());
    if (numThreads == 0) return;

    for (size_t i = 0; i < numThreads; ++i) {
        m_threads.emplace_back(&ProcessPool::workerThreadLoop, this);
    }

    for (auto& t : m_threads) {
        if (t.joinable()) t.join();
    }

    m_threads.clear();
}

void ProcessPool::workerThreadLoop() {
    while (!m_stop) {
        ScanJob job;
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            if (m_jobQueue.empty()) break;
            job = m_jobQueue.back();
            m_jobQueue.pop_back();
        }
        processJob(job);
    }
}

void ProcessPool::processJob(const ScanJob& job) {
    auto uuid = boost::uuids::random_generator()();
    std::string ipcId = "rps_ipc_" + boost::uuids::to_string(uuid);

    std::cout << "[Worker Thread] Scanning: " << job.pluginPath.filename().string() << "\n";

    try {
        auto connection = rps::ipc::MessageQueueConnection::createServer(ipcId);
        bp::child scannerProc(job.scannerBin, "--ipc-id", ipcId, "--plugin-path", job.pluginPath.string());

        rps::ipc::Message reqMsg;
        reqMsg.type = rps::ipc::MessageType::ScanRequest;
        reqMsg.payload = rps::ipc::ScanRequest{ job.pluginPath.string(), "unknown", false };
        
        if (!connection->sendMessage(reqMsg)) {
            std::cerr << "[Worker Thread Error] Failed to send ScanRequest to worker.\n";
            scannerProc.terminate();
            return;
        }

        bool done = false;
        auto startTime = std::chrono::steady_clock::now();
        auto lastResponseTime = startTime;

        while (!done && !m_stop) {
            auto maybeMsg = connection->receiveMessage(100); // 100ms polling
            auto now = std::chrono::steady_clock::now();
            
            if (maybeMsg.has_value()) {
                lastResponseTime = now; 
                const auto& msg = maybeMsg.value();
                if (msg.type == rps::ipc::MessageType::ProgressEvent) {
                    auto evt = std::get<rps::ipc::ProgressEvent>(msg.payload);
                    // Commented out to prevent console spam during parallel scan, can add verbosity flags later
                    // std::cout << "  [" << job.pluginPath.filename().string() << "] " << evt.progressPercentage << "%: " << evt.status << "\n";
                } 
                else if (msg.type == rps::ipc::MessageType::ScanResult) {
                    auto res = std::get<rps::ipc::ScanResult>(msg.payload);
                    std::cout << "[SUCCESS] " << job.pluginPath.filename().string() << " -> " << res.name << " v" << res.version << "\n";
                    if (m_db) m_db->upsertPluginResult(job.pluginPath, res);
                    done = true;
                }
                else if (msg.type == rps::ipc::MessageType::ErrorMessage) {
                    auto err = std::get<rps::ipc::ErrorMessage>(msg.payload);
                    std::cerr << "[ERROR] " << job.pluginPath.filename().string() << ": " << err.error << "\n";
                    if (m_db) m_db->recordPluginFailure(job.pluginPath, err.error + ": " + err.details);
                    done = true;
                }
            } else {
                if (!scannerProc.running()) {
                    std::string errMsg = "Process crashed (exit code: " + std::to_string(scannerProc.exit_code()) + ")";
                    std::cerr << "[CRASH] " << job.pluginPath.filename().string() << " " << errMsg << "\n";
                    if (m_db) m_db->recordPluginFailure(job.pluginPath, errMsg);
                    done = true;
                } 
                else if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastResponseTime).count() > job.timeoutMs) {
                    std::string errMsg = "Process timed out (deadlocked/hung plugin)";
                    std::cerr << "[TIMEOUT] " << job.pluginPath.filename().string() << " " << errMsg << "\n";
                    if (m_db) m_db->recordPluginFailure(job.pluginPath, errMsg);
                    scannerProc.terminate();
                    done = true;
                }
            }
        }

        if (scannerProc.running()) {
            scannerProc.wait();
        }

    } catch (const std::exception& e) {
        std::cerr << "[Worker Thread Fatal] " << job.pluginPath.filename().string() << ": " << e.what() << "\n";
    }
}

} // namespace rps::orchestrator
