#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif

#include <rps/orchestrator/ProcessPool.hpp>
#include <iostream>
#include <string>
#include <thread>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <boost/process/v1/pipe.hpp>

namespace {

std::string stripAnsiCodes(const std::string& line) {
    std::string result;
    result.reserve(line.size());
    for (size_t i = 0; i < line.size(); ) {
        if (line[i] == '\x1b' && i + 1 < line.size() && line[i + 1] == '[') {
            i += 2;
            while (i < line.size() && line[i] != 'm' && line[i] != 'K' && line[i] != 'J' && line[i] != 'H') ++i;
            if (i < line.size()) ++i;
        } else {
            result += line[i++];
        }
    }
    return result;
}

} // anonymous namespace

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

    {
        std::lock_guard<std::mutex> lock(m_consoleMutex);
        std::cout << "[Worker Thread] Scanning: " << job.pluginPath.filename().string() << "\n";
    }

    try {
        auto connection = rps::ipc::MessageQueueConnection::createServer(ipcId);

        std::vector<std::string> scanArgs = {"--ipc-id", ipcId, "--plugin-path", job.pluginPath.string()};
        if (job.verbose) scanArgs.push_back("--verbose");

        bp::ipstream errStream;
        bp::child scannerProc(job.scannerBin, bp::args(scanArgs), bp::std_err > errStream);

        const bool printVerbose = job.verbose;
        std::thread stderrDrainer([&errStream, printVerbose, this]() {
            std::string line;
            while (std::getline(errStream, line)) {
                if (printVerbose) {
                    std::lock_guard<std::mutex> lock(m_consoleMutex);
                    std::cerr << stripAnsiCodes(line) << "\n";
                }
            }
        });

        rps::ipc::Message reqMsg;
        reqMsg.type = rps::ipc::MessageType::ScanRequest;
        reqMsg.payload = rps::ipc::ScanRequest{ job.pluginPath.string(), "unknown", false };
        
        if (!connection->sendMessage(reqMsg)) {
            std::lock_guard<std::mutex> lock(m_consoleMutex);
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
                    {
                        std::lock_guard<std::mutex> lock(m_consoleMutex);
                        std::cout << "[SUCCESS] " << job.pluginPath.filename().string() << " -> " << res.name << " v" << res.version << "\n";
                    }
                    if (m_db) m_db->upsertPluginResult(job.pluginPath, res);
                    done = true;
                }
                else if (msg.type == rps::ipc::MessageType::ErrorMessage) {
                    auto err = std::get<rps::ipc::ErrorMessage>(msg.payload);
                    {
                        std::lock_guard<std::mutex> lock(m_consoleMutex);
                        std::cerr << "[ERROR] " << job.pluginPath.filename().string() << ": " << err.error << "\n";
                    }
                    if (m_db) m_db->recordPluginFailure(job.pluginPath, err.error + ": " + err.details);
                    done = true;
                }
            } else {
                if (!scannerProc.running()) {
                    std::string errMsg = "Process crashed (exit code: " + std::to_string(scannerProc.exit_code()) + ")";
                    {
                        std::lock_guard<std::mutex> lock(m_consoleMutex);
                        std::cerr << "[CRASH] " << job.pluginPath.filename().string() << " " << errMsg << "\n";
                    }
                    if (m_db) m_db->recordPluginFailure(job.pluginPath, errMsg);
                    done = true;
                } 
                else if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastResponseTime).count() > job.timeoutMs) {
                    std::string errMsg = "Process timed out (deadlocked/hung plugin)";
                    {
                        std::lock_guard<std::mutex> lock(m_consoleMutex);
                        std::cerr << "[TIMEOUT] " << job.pluginPath.filename().string() << " " << errMsg << "\n";
                    }
                    if (m_db) m_db->recordPluginFailure(job.pluginPath, errMsg);
                    scannerProc.terminate();
                    done = true;
                }
            }
        }

        if (scannerProc.running()) {
            scannerProc.wait();
        }

        stderrDrainer.join();

    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(m_consoleMutex);
        std::cerr << "[Worker Thread Fatal] " << job.pluginPath.filename().string() << ": " << e.what() << "\n";
    }
}

} // namespace rps::orchestrator
