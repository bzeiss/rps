#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
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

std::string describeExitCode([[maybe_unused]] int code) {
#ifdef _WIN32
    // Common Windows NTSTATUS / exception codes
    auto u = static_cast<unsigned int>(code);
    switch (u) {
        case 0xC0000005: return "ACCESS_VIOLATION";
        case 0xC00000FD: return "STACK_OVERFLOW";
        case 0xC0000094: return "INTEGER_DIVIDE_BY_ZERO";
        case 0xC000001D: return "ILLEGAL_INSTRUCTION";
        case 0xC0000374: return "HEAP_CORRUPTION";
        case 0x80000003: return "BREAKPOINT";
        case 0xC00000FE: return "STACK_BUFFER_OVERRUN";
        default: break;
    }
#endif
    return {};
}

} // anonymous namespace

namespace rps::orchestrator {

namespace bp = boost::process::v1;

ProcessPool::ProcessPool(size_t maxWorkers, db::DatabaseManager* db)
    : m_maxWorkers(maxWorkers), m_db(db) {}

ProcessPool::ScanStats ProcessPool::stats() const {
    return { m_success.load(), m_fail.load(), m_crash.load(), m_timeout.load() };
}

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
        m_threads.emplace_back(&ProcessPool::workerThreadLoop, this, i + 1);
    }

    for (auto& t : m_threads) {
        if (t.joinable()) t.join();
    }

    m_threads.clear();
}

std::string ProcessPool::formatDuration(int64_t ms) {
    if (ms < 1000) {
        return std::to_string(ms) + "ms";
    } else if (ms < 60000) {
        auto s = ms / 1000;
        auto rem = ms % 1000;
        return std::to_string(s) + "s " + std::to_string(rem) + "ms";
    } else {
        auto m = ms / 60000;
        auto s = (ms % 60000) / 1000;
        auto rem = ms % 1000;
        return std::to_string(m) + "m " + std::to_string(s) + "s " + std::to_string(rem) + "ms";
    }
}

void ProcessPool::workerThreadLoop(size_t workerId) {
    while (!m_stop) {
        ScanJob job;
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            if (m_jobQueue.empty()) break;
            job = m_jobQueue.back();
            m_jobQueue.pop_back();
        }
        processJob(job, workerId);
    }
}

void ProcessPool::processJob(const ScanJob& job, size_t workerId) {
    auto uuid = boost::uuids::random_generator()();
    std::string ipcId = "rps_ipc_" + boost::uuids::to_string(uuid);

    std::string pluginName = job.pluginPath.filename().string();
    std::string pluginFullPath = job.pluginPath.string();
    {
        std::lock_guard<std::mutex> lock(m_consoleMutex);
        std::cout << "[Worker #" << workerId << "] Scanning: " << pluginFullPath << "\n";
    }

    try {
        auto connection = rps::ipc::MessageQueueConnection::createServer(ipcId);

        std::vector<std::string> scanArgs = {"--ipc-id", ipcId, "--plugin-path", job.pluginPath.string()};
        if (job.verbose) scanArgs.push_back("--verbose");

        bp::ipstream errStream;
        bp::child scannerProc(job.scannerBin, bp::args(scanArgs), bp::std_err > errStream);

#ifdef _WIN32
        // Restrict scanner child process from showing Win32 UI (dialogs, message boxes).
        // JOB_OBJECT_UILIMIT_HANDLES prevents the process from using USER handles (HWNDs)
        // owned by processes outside the job - this silently suppresses MessageBox calls
        // from plugin DLLs (e.g. Kilohearts session mutex dialog) without affecting
        // pipe/kernel handles used for IPC.
        {
            HANDLE hJob = CreateJobObject(nullptr, nullptr);
            if (hJob) {
                JOBOBJECT_BASIC_UI_RESTRICTIONS uiRestr = {};
                uiRestr.UIRestrictionsClass =
                    JOB_OBJECT_UILIMIT_HANDLES |
                    JOB_OBJECT_UILIMIT_GLOBALATOMS |
                    JOB_OBJECT_UILIMIT_READCLIPBOARD |
                    JOB_OBJECT_UILIMIT_WRITECLIPBOARD |
                    JOB_OBJECT_UILIMIT_SYSTEMPARAMETERS |
                    JOB_OBJECT_UILIMIT_EXITWINDOWS;
                SetInformationJobObject(hJob, JobObjectBasicUIRestrictions,
                    &uiRestr, sizeof(uiRestr));
                AssignProcessToJobObject(hJob, scannerProc.native_handle());
                CloseHandle(hJob);
            }
        }
#endif

        const bool printVerbose = job.verbose;
        std::vector<std::string> stderrLines;
        std::mutex stderrMutex;
        std::thread stderrDrainer([&errStream, &stderrLines, &stderrMutex, printVerbose, workerId, this]() {
            std::string line;
            while (std::getline(errStream, line)) {
                std::string cleaned = stripAnsiCodes(line);
                {
                    std::lock_guard<std::mutex> lock(stderrMutex);
                    stderrLines.push_back(cleaned);
                }
                if (printVerbose) {
                    std::lock_guard<std::mutex> lock(m_consoleMutex);
                    std::cerr << "[Worker #" << workerId << " DBG] " << cleaned << "\n";
                }
            }
        });

        rps::ipc::Message reqMsg;
        reqMsg.type = rps::ipc::MessageType::ScanRequest;
        reqMsg.payload = rps::ipc::ScanRequest{ job.pluginPath.string(), "unknown", false };
        
        if (!connection->sendMessage(reqMsg)) {
            std::lock_guard<std::mutex> lock(m_consoleMutex);
            std::cerr << "[Worker #" << workerId << " ERROR] Failed to send ScanRequest for " << pluginName << "\n";
            scannerProc.terminate();
            return;
        }

        bool done = false;
        bool slowWarned = false;
        constexpr int64_t slowThresholdMs = 30000; // 30 seconds
        auto startTime = std::chrono::steady_clock::now();
        auto lastResponseTime = startTime;

        while (!done && !m_stop) {
            auto maybeMsg = connection->receiveMessage(100); // 100ms polling
            auto now = std::chrono::steady_clock::now();

            // Warn once if scan is taking unusually long
            if (!slowWarned) {
                auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime).count();
                if (elapsedMs > slowThresholdMs) {
                    slowWarned = true;
                    std::lock_guard<std::mutex> lock(m_consoleMutex);
                    std::cerr << "[Worker #" << workerId << " SLOW] " << pluginName
                              << " still scanning after " << formatDuration(elapsedMs) << "...\n";
                }
            }
            
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
                    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime).count();
                    {
                        std::lock_guard<std::mutex> lock(m_consoleMutex);
                        std::cout << "[Worker #" << workerId << " SUCCESS] " << pluginName
                                  << " -> " << res.name << " v" << res.version 
                                  << " (" << formatDuration(elapsedMs) << ")\n";
                        if (printVerbose) {
                            std::cout << "  Method:   " << (res.scanMethod.empty() ? "unknown" : res.scanMethod) << "\n";
                            std::cout << "  Vendor:   " << (res.vendor.empty() ? "(none)" : res.vendor) << "\n";
                            std::cout << "  UID:      " << (res.uid.empty() ? "(none)" : res.uid) << "\n";
                            std::cout << "  Category: " << (res.category.empty() ? "(none)" : res.category) << "\n";
                            std::cout << "  URL:      " << (res.url.empty() ? "(none)" : res.url) << "\n";
                            std::cout << "  Desc:     " << (res.description.empty() ? "(none)" : res.description) << "\n";
                            std::cout << "  I/O:      " << (res.numInputs || res.numOutputs
                                ? std::to_string(res.numInputs) + " in / " + std::to_string(res.numOutputs) + " out"
                                : "skipped (requires instantiation)") << "\n";
                            std::cout << "  Params:   " << (res.parameters.empty()
                                ? "skipped (requires instantiation)"
                                : std::to_string(res.parameters.size()) + " parameter(s)") << "\n";
                        }
                    }
                    if (m_db) m_db->upsertPluginResult(job.pluginPath, res, elapsedMs);
                    ++m_success;
                    done = true;
                }
                else if (msg.type == rps::ipc::MessageType::ErrorMessage) {
                    auto err = std::get<rps::ipc::ErrorMessage>(msg.payload);
                    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime).count();
                    {
                        std::lock_guard<std::mutex> lock(m_consoleMutex);
                        std::cerr << "[Worker #" << workerId << " ERROR] " << pluginName
                                  << ": " << err.error 
                                  << " (" << formatDuration(elapsedMs) << ")\n";
                        if (!printVerbose) {
                            std::lock_guard<std::mutex> slock(stderrMutex);
                            for (const auto& l : stderrLines) {
                                std::cerr << "  [Worker #" << workerId << " stderr] " << l << "\n";
                            }
                        }
                    }
                    if (m_db) m_db->recordPluginFailure(job.pluginPath, err.error + ": " + err.details, elapsedMs);
                    ++m_fail;
                    done = true;
                }
            } else {
                if (!scannerProc.running()) {
                    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime).count();
                    int exitCode = scannerProc.exit_code();
                    bool isHardCrash = (exitCode < 0 || exitCode > 1);
                    std::string tag = isHardCrash ? "CRASH" : "FAIL";
                    std::string codeDesc = describeExitCode(exitCode);
                    std::string errMsg = isHardCrash
                        ? "Process crashed (exit code: " + std::to_string(exitCode)
                        : "Scanner reported error (exit code: " + std::to_string(exitCode);
                    if (!codeDesc.empty()) errMsg += " = " + codeDesc;
                    errMsg += ")";
                    {
                        std::lock_guard<std::mutex> lock(m_consoleMutex);
                        std::cerr << "[Worker #" << workerId << " " << tag << "] " << pluginName
                                  << " " << errMsg 
                                  << " (" << formatDuration(elapsedMs) << ")\n";
                        // Dump captured stderr on crash (skip if already printed in verbose mode)
                        if (!printVerbose) {
                            std::lock_guard<std::mutex> slock(stderrMutex);
                            for (const auto& l : stderrLines) {
                                std::cerr << "  [Worker #" << workerId << " stderr] " << l << "\n";
                            }
                        }
                    }
                    if (m_db) m_db->recordPluginFailure(job.pluginPath, errMsg, elapsedMs);
                    if (isHardCrash) ++m_crash; else ++m_fail;
                    done = true;
                } 
                else if (job.timeoutMs > 0 && std::chrono::duration_cast<std::chrono::milliseconds>(now - lastResponseTime).count() > job.timeoutMs) {
                    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime).count();
                    std::string errMsg = "Process timed out (deadlocked/hung plugin)";
                    {
                        std::lock_guard<std::mutex> lock(m_consoleMutex);
                        std::cerr << "[Worker #" << workerId << " TIMEOUT] " << pluginName
                                  << " " << errMsg 
                                  << " (" << formatDuration(elapsedMs) << ")\n";
                        if (!printVerbose) {
                            std::lock_guard<std::mutex> slock(stderrMutex);
                            for (const auto& l : stderrLines) {
                                std::cerr << "  [Worker #" << workerId << " stderr] " << l << "\n";
                            }
                        }
                    }
                    if (m_db) m_db->recordPluginFailure(job.pluginPath, errMsg, elapsedMs);
                    ++m_timeout;
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
        std::cerr << "[Worker #" << workerId << " FATAL] " << pluginName << ": " << e.what() << "\n";
    }
}

} // namespace rps::orchestrator
