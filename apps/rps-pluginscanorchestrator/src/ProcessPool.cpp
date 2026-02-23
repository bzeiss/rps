#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <rps/orchestrator/ProcessPool.hpp>
#include <string>
#include <iostream>
#include <algorithm>
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

ProcessPool::ProcessPool(size_t maxWorkers, db::DatabaseManager* db, ScanObserver* observer)
    : m_maxWorkers(maxWorkers), m_db(db), m_observer(observer) {}

ProcessPool::ScanStats ProcessPool::stats() const {
    return { m_success.load(), m_fail.load(), m_crash.load(), m_timeout.load(), m_skipped.load() };
}

std::vector<std::pair<std::string, std::string>> ProcessPool::failures() const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(m_failureMutex));
    return m_failures;
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

    // Monitor thread: periodically report still-active workers
    std::atomic<bool> monitorStop{false};
    std::thread monitor([this, &monitorStop]() {
        while (!monitorStop) {
            for (int i = 0; i < 300 && !monitorStop; ++i)  // 30s in 100ms ticks
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (monitorStop) break;

            std::lock_guard<std::mutex> aLock(m_activeMutex);
            if (!m_activeWorkers.empty() && m_observer) {
                auto now = std::chrono::steady_clock::now();
                std::vector<std::pair<size_t, std::pair<std::string, int64_t>>> report;
                for (auto& [id, info] : m_activeWorkers) {
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - info.startTime).count();
                    report.push_back({id, {info.pluginPath, elapsed}});
                }
                m_observer->onMonitorReport(report);
            }
        }
    });

    for (auto& t : m_threads) {
        if (t.joinable()) t.join();
    }

    monitorStop = true;
    monitor.join();
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

void ProcessPool::recordFailure(const std::string& pluginPath, const std::string& reason) {
    std::lock_guard<std::mutex> lock(m_failureMutex);
    // Update existing entry or add new one
    for (auto& [p, r] : m_failures) {
        if (p == pluginPath) { r = reason; return; }
    }
    m_failures.push_back({pluginPath, reason});
}

bool ProcessPool::enqueueRetry(const ScanJob& job, const std::string& reason, size_t workerId) {
    if (job.attempt >= job.maxRetries) return false;
    ScanJob retry = job;
    retry.attempt = job.attempt + 1;
    {
        std::lock_guard<std::mutex> lock(m_retryMutex);
        m_retryQueue.push_back(retry);
    }
    if (m_observer) {
        m_observer->onPluginRetry(workerId, job.pluginPath.string(), retry.attempt, job.maxRetries, reason);
    }
    return true;
}

void ProcessPool::workerThreadLoop(size_t workerId) {
    while (!m_stop) {
        ScanJob job;
        bool gotJob = false;
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            if (!m_jobQueue.empty()) {
                job = m_jobQueue.back();
                m_jobQueue.pop_back();
                gotJob = true;
            }
        }
        if (!gotJob) {
            // Try retry queue
            std::lock_guard<std::mutex> lock(m_retryMutex);
            if (!m_retryQueue.empty()) {
                job = m_retryQueue.back();
                m_retryQueue.pop_back();
                gotJob = true;
            }
        }
        if (!gotJob) {
            // Both queues empty — but other workers may still be processing
            // jobs that could fail and produce retries. Wait briefly and check again.
            bool othersActive = false;
            {
                std::lock_guard<std::mutex> lock(m_activeMutex);
                // Check if any OTHER worker is still active
                for (auto& [id, info] : m_activeWorkers) {
                    if (id != workerId) { othersActive = true; break; }
                }
            }
            if (othersActive) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;  // Re-check queues
            }
            break;  // No other workers active and queues empty — done
        }
        processJob(job, workerId);
    }
}

void ProcessPool::processJob(const ScanJob& job, size_t workerId) {
    auto uuid = boost::uuids::random_generator()();
    std::string ipcId = "rps_ipc_" + boost::uuids::to_string(uuid);

    std::string pluginName = job.pluginPath.filename().string();
    std::string pluginFullPath = job.pluginPath.string();

    // Pre-compute file metadata for DB storage
    std::string fileMtime = db::DatabaseManager::getFileMtime(job.pluginPath);
    std::string fileHash = db::DatabaseManager::computeFileHash(job.pluginPath);
    if (m_observer) {
        m_observer->onPluginStarted(workerId, job.pluginIndex, job.totalPlugins, pluginFullPath);
    }
    {
        std::lock_guard<std::mutex> lock(m_activeMutex);
        m_activeWorkers[workerId] = {pluginFullPath, std::chrono::steady_clock::now()};
    }

    auto deregisterWorker = [&]() {
        std::lock_guard<std::mutex> lock(m_activeMutex);
        m_activeWorkers.erase(workerId);
    };

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

        std::vector<std::string> stderrLines;
        std::mutex stderrMutex;
        std::thread stderrDrainer([&errStream, &stderrLines, &stderrMutex, workerId, &pluginFullPath, this]() {
            std::string line;
            while (std::getline(errStream, line)) {
                std::string cleaned = stripAnsiCodes(line);
                {
                    std::lock_guard<std::mutex> lock(stderrMutex);
                    stderrLines.push_back(cleaned);
                }
                if (m_observer) {
                    m_observer->onWorkerStderrLine(workerId, pluginFullPath, cleaned);
                }
            }
        });

        // Helper: safely join the stderr drainer. Must be called before
        // stderrDrainer goes out of scope, or std::terminate() is triggered.
        auto safeJoinDrainer = [&]() {
            try { errStream.pipe().close(); } catch (...) {}
            if (stderrDrainer.joinable()) stderrDrainer.join();
        };

        // Everything from here until safeJoinDrainer() is wrapped in a try-catch
        // that ensures the drainer is joined before any exception propagates.
        // Without this, stack unwinding would destroy stderrDrainer while still
        // joinable, triggering std::terminate().
        try {

        rps::ipc::Message reqMsg;
        reqMsg.type = rps::ipc::MessageType::ScanRequest;
        reqMsg.payload = rps::ipc::ScanRequest{ job.pluginPath.string(), "unknown", false };
        
        if (!connection->sendMessage(reqMsg)) {
            std::string errMsg = "Failed to send ScanRequest";
            if (!enqueueRetry(job, errMsg, workerId)) {
                if (m_observer) {
                    m_observer->onPluginCompleted(workerId, pluginFullPath, ScanOutcome::Fail, 0, nullptr, &errMsg);
                }
                recordFailure(pluginFullPath, errMsg);
                ++m_fail;
            }
            scannerProc.terminate();
            safeJoinDrainer();
            deregisterWorker();
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
                    if (m_observer) {
                        m_observer->onPluginSlowWarning(workerId, pluginFullPath, elapsedMs);
                    }
                }
            }
            
            if (maybeMsg.has_value()) {
                lastResponseTime = now; 
                const auto& msg = maybeMsg.value();
                if (msg.type == rps::ipc::MessageType::ProgressEvent) {
                    auto evt = std::get<rps::ipc::ProgressEvent>(msg.payload);
                    if (m_observer) {
                        m_observer->onPluginProgress(workerId, pluginFullPath, evt.progressPercentage, evt.status);
                    }
                } 
                else if (msg.type == rps::ipc::MessageType::ScanResult) {
                    auto res = std::get<rps::ipc::ScanResult>(msg.payload);
                    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime).count();
                    if (m_observer) {
                        m_observer->onPluginCompleted(workerId, pluginFullPath, ScanOutcome::Success, elapsedMs, &res, nullptr);
                    }
                    if (m_db) {
                        auto dbT0 = std::chrono::steady_clock::now();
                        m_db->upsertPluginResult(job.pluginPath, res, elapsedMs, fileMtime, fileHash);
                        auto dbMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - dbT0).count();
                        if (job.verbose) {
                            std::cerr << "[Worker #" << workerId << " DBG] DB upsert: "
                                      << dbMs << "ms (" << res.parameters.size() << " params)\n";
                        }
                    }
                    // Remove from failures list if this was a successful retry
                    {
                        std::lock_guard<std::mutex> flock(m_failureMutex);
                        m_failures.erase(
                            std::remove_if(m_failures.begin(), m_failures.end(),
                                [&](const auto& f) { return f.first == pluginFullPath; }),
                            m_failures.end());
                    }
                    ++m_success;
                    done = true;
                }
                else if (msg.type == rps::ipc::MessageType::ErrorMessage) {
                    auto err = std::get<rps::ipc::ErrorMessage>(msg.payload);
                    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime).count();
                    std::string errMsg = err.error + ": " + err.details;
                    // Detect SKIP: prefix — plugin is not scannable (e.g. empty bundle)
                    bool isSkip = err.details.rfind("SKIP:", 0) == 0;
                    if (isSkip) {
                        if (m_observer) {
                            m_observer->onPluginCompleted(workerId, pluginFullPath, ScanOutcome::Skipped, elapsedMs, nullptr, &errMsg);
                        }
                        if (m_db) m_db->recordPluginSkip(job.pluginPath, job.format, err.details, fileMtime);
                        ++m_skipped;
                    } else if (!enqueueRetry(job, errMsg, workerId)) {
                        if (m_observer) {
                            m_observer->onPluginCompleted(workerId, pluginFullPath, ScanOutcome::Fail, elapsedMs, nullptr, &errMsg);
                            std::lock_guard<std::mutex> slock(stderrMutex);
                            m_observer->onWorkerStderrDump(workerId, pluginFullPath, stderrLines);
                        }
                        if (m_db) {
                            m_db->recordPluginFailure(job.pluginPath, errMsg, elapsedMs, fileMtime, fileHash);
                            m_db->recordPluginBlocked(job.pluginPath, job.format, errMsg, fileMtime);
                        }
                        recordFailure(pluginFullPath, errMsg);
                        ++m_fail;
                    }
                    done = true;
                }
            } else {
                if (!scannerProc.running()) {
                    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime).count();
                    int exitCode = scannerProc.exit_code();
                    bool isHardCrash = (exitCode < 0 || exitCode > 1);
                    std::string codeDesc = describeExitCode(exitCode);
                    std::string errMsg = isHardCrash
                        ? "Process crashed (exit code: " + std::to_string(exitCode)
                        : "Scanner reported error (exit code: " + std::to_string(exitCode);
                    if (!codeDesc.empty()) errMsg += " = " + codeDesc;
                    errMsg += ")";
                    ScanOutcome outcome = isHardCrash ? ScanOutcome::Crash : ScanOutcome::Fail;
                    if (!enqueueRetry(job, errMsg, workerId)) {
                        if (m_observer) {
                            m_observer->onPluginCompleted(workerId, pluginFullPath, outcome, elapsedMs, nullptr, &errMsg);
                            std::lock_guard<std::mutex> slock(stderrMutex);
                            m_observer->onWorkerStderrDump(workerId, pluginFullPath, stderrLines);
                        }
                        if (m_db) {
                            m_db->recordPluginFailure(job.pluginPath, errMsg, elapsedMs, fileMtime, fileHash);
                            m_db->recordPluginBlocked(job.pluginPath, job.format, errMsg, fileMtime);
                        }
                        recordFailure(pluginFullPath, errMsg);
                        if (isHardCrash) ++m_crash; else ++m_fail;
                    }
                    done = true;
                } 
                else if (job.timeoutMs > 0 && std::chrono::duration_cast<std::chrono::milliseconds>(now - lastResponseTime).count() > job.timeoutMs) {
                    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime).count();
                    std::string errMsg = "Process timed out (deadlocked/hung plugin)";
                    // Don't retry timeouts — they'll just timeout again
                    if (m_observer) {
                        m_observer->onPluginCompleted(workerId, pluginFullPath, ScanOutcome::Timeout, elapsedMs, nullptr, &errMsg);
                        std::lock_guard<std::mutex> slock(stderrMutex);
                        m_observer->onWorkerStderrDump(workerId, pluginFullPath, stderrLines);
                    }
                    if (m_db) {
                        m_db->recordPluginFailure(job.pluginPath, errMsg, elapsedMs, fileMtime, fileHash);
                        m_db->recordPluginBlocked(job.pluginPath, job.format, errMsg, fileMtime);
                    }
                    recordFailure(pluginFullPath, errMsg);
                    ++m_timeout;
                    scannerProc.terminate();
                    done = true;
                }
            }
        }

        // --- Post-scan cleanup ---
        // stderrDrainer MUST be joined before it goes out of scope, otherwise
        // std::terminate() is called (destroying a joinable thread is UB).
        // Wrap cleanup in its own try-catch to guarantee the join.
        try {
            auto cleanupT0 = std::chrono::steady_clock::now();
            auto logCleanup = [&](const char* step) {
                if (job.verbose) {
                    auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - cleanupT0).count();
                    std::cerr << "[Worker #" << workerId << " DBG] cleanup: " << step
                              << " (+" << dt << "ms) running=" << scannerProc.running() << "\n";
                }
            };

            logCleanup("start");

            if (scannerProc.running()) {
                logCleanup("process still running, terminating");
                scannerProc.terminate();
                logCleanup("terminate() returned");
            }

            logCleanup("calling wait()");
            scannerProc.wait();
            logCleanup("wait() returned");
        } catch (const std::exception& ex) {
            std::cerr << "[Worker #" << workerId << " WARN] cleanup exception: " << ex.what() << "\n";
        }

        // Always close the pipe and join the drainer, even if the above failed
        safeJoinDrainer();

        } catch (...) {
            // Join drainer before re-throwing so ~thread doesn't call std::terminate
            safeJoinDrainer();
            throw;
        }

    } catch (const std::exception& e) {
        std::string errMsg = std::string("FATAL: ") + e.what();
        if (!enqueueRetry(job, errMsg, workerId)) {
            if (m_observer) {
                m_observer->onPluginCompleted(workerId, pluginFullPath, ScanOutcome::Crash, 0, nullptr, &errMsg);
            }
            recordFailure(pluginFullPath, errMsg);
            ++m_crash;
        }
    } catch (...) {
        // Catch non-std exceptions to prevent std::terminate
        std::string errMsg = "FATAL: Unknown exception";
        if (!enqueueRetry(job, errMsg, workerId)) {
            if (m_observer) {
                m_observer->onPluginCompleted(workerId, pluginFullPath, ScanOutcome::Crash, 0, nullptr, &errMsg);
            }
            recordFailure(pluginFullPath, errMsg);
            ++m_crash;
        }
    }
    deregisterWorker();
}

} // namespace rps::orchestrator
