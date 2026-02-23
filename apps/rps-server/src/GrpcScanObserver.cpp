#include <rps/server/GrpcScanObserver.hpp>
#include <rps.pb.h>
#include <rps.grpc.pb.h>
#include <boost/filesystem.hpp>
#include <spdlog/spdlog.h>

namespace rps::server {

GrpcScanObserver::GrpcScanObserver(grpc::ServerWriter<rps::v1::ScanEvent>* writer)
    : m_writer(writer) {}

void GrpcScanObserver::setIncrementalStats(uint32_t skippedUnchanged, uint32_t skippedBlocked) {
    m_skippedUnchanged = skippedUnchanged;
    m_skippedBlocked = skippedBlocked;
}

void GrpcScanObserver::onScanStarted(size_t totalPlugins, size_t workerCount) {
    std::lock_guard<std::mutex> lock(m_mutex);
    rps::v1::ScanEvent event;
    auto* started = event.mutable_scan_started();
    started->set_total_plugins(static_cast<uint32_t>(totalPlugins));
    started->set_worker_count(static_cast<uint32_t>(workerCount));
    started->set_skipped_unchanged(m_skippedUnchanged);
    started->set_skipped_blocked(m_skippedBlocked);
    m_writer->Write(event);
    spdlog::info("Scan started: {} plugins, {} workers", totalPlugins, workerCount);
}

void GrpcScanObserver::onPluginStarted(size_t workerId, size_t pluginIndex, size_t totalPlugins,
                                         const std::string& pluginPath) {
    std::lock_guard<std::mutex> lock(m_mutex);
    rps::v1::ScanEvent event;
    auto* ps = event.mutable_plugin_started();
    ps->set_worker_id(static_cast<uint32_t>(workerId));
    ps->set_plugin_index(static_cast<uint32_t>(pluginIndex));
    ps->set_total_plugins(static_cast<uint32_t>(totalPlugins));
    ps->set_plugin_path(pluginPath);
    ps->set_plugin_filename(boost::filesystem::path(pluginPath).filename().string());
    m_writer->Write(event);
    spdlog::debug("[Worker #{}] Scanning: {}", workerId, pluginPath);
}

void GrpcScanObserver::onPluginProgress(size_t workerId, const std::string& pluginPath,
                                          int percentage, const std::string& stage) {
    std::lock_guard<std::mutex> lock(m_mutex);
    rps::v1::ScanEvent event;
    auto* pp = event.mutable_plugin_progress();
    pp->set_worker_id(static_cast<uint32_t>(workerId));
    pp->set_plugin_path(pluginPath);
    pp->set_percentage(percentage);
    pp->set_stage(stage);
    m_writer->Write(event);
}

void GrpcScanObserver::onPluginSlowWarning(size_t workerId, const std::string& pluginPath,
                                             int64_t elapsedMs) {
    spdlog::warn("[Worker #{}] {} still scanning after {}ms", workerId,
                 boost::filesystem::path(pluginPath).filename().string(), elapsedMs);
}

void GrpcScanObserver::onPluginCompleted(size_t workerId, const std::string& pluginPath,
                                           rps::engine::ScanOutcome outcome, int64_t elapsedMs,
                                           const rps::ipc::ScanResult* result,
                                           const std::string* errorMessage) {
    std::lock_guard<std::mutex> lock(m_mutex);
    rps::v1::ScanEvent event;
    auto* pc = event.mutable_plugin_completed();
    pc->set_worker_id(static_cast<uint32_t>(workerId));
    pc->set_plugin_path(pluginPath);
    pc->set_plugin_filename(boost::filesystem::path(pluginPath).filename().string());
    pc->set_elapsed_ms(elapsedMs);

    switch (outcome) {
        case rps::engine::ScanOutcome::Success:
            pc->set_outcome(rps::v1::OUTCOME_SUCCESS);
            if (result) {
                pc->set_plugin_name(result->name);
                pc->set_plugin_version(result->version);
                pc->set_plugin_vendor(result->vendor);
                pc->set_plugin_format(result->format);
            }
            spdlog::info("[Worker #{}] SUCCESS {} -> {} ({}ms)", workerId,
                         pc->plugin_filename(), result ? result->name : "?", elapsedMs);
            break;
        case rps::engine::ScanOutcome::Fail:
            pc->set_outcome(rps::v1::OUTCOME_FAIL);
            if (errorMessage) pc->set_error_message(*errorMessage);
            spdlog::warn("[Worker #{}] FAIL {} {} ({}ms)", workerId,
                         pc->plugin_filename(), errorMessage ? *errorMessage : "", elapsedMs);
            break;
        case rps::engine::ScanOutcome::Crash:
            pc->set_outcome(rps::v1::OUTCOME_CRASH);
            if (errorMessage) pc->set_error_message(*errorMessage);
            spdlog::error("[Worker #{}] CRASH {} {} ({}ms)", workerId,
                          pc->plugin_filename(), errorMessage ? *errorMessage : "", elapsedMs);
            break;
        case rps::engine::ScanOutcome::Timeout:
            pc->set_outcome(rps::v1::OUTCOME_TIMEOUT);
            if (errorMessage) pc->set_error_message(*errorMessage);
            spdlog::error("[Worker #{}] TIMEOUT {} ({}ms)", workerId, pc->plugin_filename(), elapsedMs);
            break;
        case rps::engine::ScanOutcome::Skipped:
            pc->set_outcome(rps::v1::OUTCOME_SKIPPED);
            if (errorMessage) pc->set_error_message(*errorMessage);
            spdlog::info("[Worker #{}] SKIP {}", workerId, pc->plugin_filename());
            break;
    }

    m_writer->Write(event);
}

void GrpcScanObserver::onWorkerStderrLine(size_t workerId, const std::string& /*pluginPath*/,
                                            const std::string& line) {
    std::lock_guard<std::mutex> lock(m_mutex);
    rps::v1::ScanEvent event;
    auto* wl = event.mutable_worker_log();
    wl->set_worker_id(static_cast<uint32_t>(workerId));
    wl->set_line(line);
    m_writer->Write(event);
    spdlog::debug("[Worker #{}] {}", workerId, line);
}

void GrpcScanObserver::onWorkerStderrDump(size_t workerId, const std::string& /*pluginPath*/,
                                            const std::vector<std::string>& lines) {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& line : lines) {
        rps::v1::ScanEvent event;
        auto* wl = event.mutable_worker_log();
        wl->set_worker_id(static_cast<uint32_t>(workerId));
        wl->set_line(line);
        m_writer->Write(event);
    }
}

void GrpcScanObserver::onWorkerForceKill(size_t workerId, const std::string& pluginPath) {
    spdlog::warn("[Worker #{}] Force-killing scanner for {}",
                 workerId, boost::filesystem::path(pluginPath).filename().string());
}

void GrpcScanObserver::onPluginRetry(size_t workerId, const std::string& pluginPath,
                                       size_t attempt, size_t maxRetries, const std::string& reason) {
    std::lock_guard<std::mutex> lock(m_mutex);
    rps::v1::ScanEvent event;
    auto* pr = event.mutable_plugin_retry();
    pr->set_worker_id(static_cast<uint32_t>(workerId));
    pr->set_plugin_path(pluginPath);
    pr->set_attempt(static_cast<uint32_t>(attempt));
    pr->set_max_retries(static_cast<uint32_t>(maxRetries));
    pr->set_reason(reason);
    m_writer->Write(event);
    spdlog::info("[Worker #{}] RETRY {} ({}/{}) {}", workerId,
                 boost::filesystem::path(pluginPath).filename().string(), attempt, maxRetries, reason);
}

void GrpcScanObserver::onMonitorReport(
    const std::vector<std::pair<size_t, std::pair<std::string, int64_t>>>& activeWorkers) {
    std::lock_guard<std::mutex> lock(m_mutex);
    rps::v1::ScanEvent event;
    auto* mr = event.mutable_monitor_report();
    for (const auto& [id, info] : activeWorkers) {
        auto* aw = mr->add_workers();
        aw->set_worker_id(static_cast<uint32_t>(id));
        aw->set_plugin_path(info.first);
        aw->set_elapsed_ms(info.second);
    }
    m_writer->Write(event);
}

void GrpcScanObserver::onScanCompleted(size_t success, size_t fail, size_t crash, size_t timeout,
                                         size_t skipped, int64_t totalMs,
                                         const std::vector<std::pair<std::string, std::string>>& failures) {
    std::lock_guard<std::mutex> lock(m_mutex);
    rps::v1::ScanEvent event;
    auto* sc = event.mutable_scan_completed();
    sc->set_success(static_cast<uint32_t>(success));
    sc->set_fail(static_cast<uint32_t>(fail));
    sc->set_crash(static_cast<uint32_t>(crash));
    sc->set_timeout(static_cast<uint32_t>(timeout));
    sc->set_skipped(static_cast<uint32_t>(skipped));
    sc->set_total_ms(totalMs);
    for (const auto& [path, reason] : failures) {
        auto* fp = sc->add_failures();
        fp->set_path(path);
        fp->set_reason(reason);
    }
    m_writer->Write(event);
    spdlog::info("Scan completed: {} success, {} fail, {} crash, {} timeout, {} skipped ({}ms)",
                 success, fail, crash, timeout, skipped, totalMs);
}

} // namespace rps::server
