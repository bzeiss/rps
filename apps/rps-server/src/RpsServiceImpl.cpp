#include <rps/server/RpsServiceImpl.hpp>
#include <rps/server/GrpcScanObserver.hpp>
#include <spdlog/spdlog.h>

namespace rps::server {

RpsServiceImpl::RpsServiceImpl(const std::string& dbPath, const std::string& scannerBin)
    : m_dbPath(dbPath), m_scannerBin(scannerBin), m_startTime(std::chrono::steady_clock::now()) {}

void RpsServiceImpl::setServer(grpc::Server* server) {
    std::lock_guard<std::mutex> lock(m_serverMutex);
    m_server = server;
}

grpc::Status RpsServiceImpl::StartScan(grpc::ServerContext* /*context*/,
                                        const rps::v1::StartScanRequest* request,
                                        grpc::ServerWriter<rps::v1::ScanEvent>* writer) {
    if (m_engine.isScanning()) {
        return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "A scan is already running");
    }

    // Build ScanConfig from the gRPC request
    rps::engine::ScanConfig config;
    for (const auto& dir : request->scan_dirs()) {
        config.scanDirs.push_back(dir);
    }
    config.singlePlugin = request->single_plugin();
    config.mode = request->mode().empty() ? "incremental" : request->mode();
    config.formats = request->formats().empty() ? "all" : request->formats();
    config.filter = request->filter();
    config.limit = request->limit();
    config.jobs = request->jobs() == 0 ? 6 : request->jobs();
    config.retries = request->retries() == 0 ? 3 : request->retries();
    config.timeoutMs = request->timeout_ms() == 0 ? 120000 : static_cast<int>(request->timeout_ms());
    config.verbose = request->verbose();
    config.dbPath = m_dbPath;
    config.scannerBin = m_scannerBin;

    spdlog::info("StartScan: mode={} formats={} jobs={} timeout={}ms",
                 config.mode, config.formats, config.jobs, config.timeoutMs);

    GrpcScanObserver observer(writer);
    auto summary = m_engine.runScan(config, &observer);

    spdlog::info("Scan finished: {} success, {} fail, {} crash, {} timeout, {} skipped ({}ms)",
                 summary.success, summary.fail, summary.crash, summary.timeout,
                 summary.skipped, summary.totalMs);

    return grpc::Status::OK;
}

grpc::Status RpsServiceImpl::StopScan(grpc::ServerContext* /*context*/,
                                       const rps::v1::StopScanRequest* /*request*/,
                                       rps::v1::StopScanResponse* response) {
    bool wasRunning = m_engine.isScanning();
    response->set_was_running(wasRunning);
    if (wasRunning) {
        m_engine.stop();
    }
    spdlog::info("StopScan requested (was_running={})", wasRunning);
    return grpc::Status::OK;
}

grpc::Status RpsServiceImpl::GetStatus(grpc::ServerContext* /*context*/,
                                        const rps::v1::GetStatusRequest* /*request*/,
                                        rps::v1::GetStatusResponse* response) {
    auto now = std::chrono::steady_clock::now();
    auto uptimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_startTime).count();

    response->set_state(m_engine.isScanning()
        ? rps::v1::GetStatusResponse::SCANNING
        : rps::v1::GetStatusResponse::IDLE);
    response->set_uptime_ms(uptimeMs);
    response->set_db_path(m_dbPath);
    return grpc::Status::OK;
}

grpc::Status RpsServiceImpl::Shutdown(grpc::ServerContext* /*context*/,
                                       const rps::v1::ShutdownRequest* /*request*/,
                                       rps::v1::ShutdownResponse* /*response*/) {
    spdlog::info("Shutdown requested");
    m_engine.stop();
    std::lock_guard<std::mutex> lock(m_serverMutex);
    if (m_server) {
        // Shutdown asynchronously so we can return the response first
        std::thread([this]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            std::lock_guard<std::mutex> lock(m_serverMutex);
            if (m_server) {
                auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(2);
                m_server->Shutdown(deadline);
            }
        }).detach();
    }
    return grpc::Status::OK;
}

void RpsServiceImpl::stopScan() {
    m_engine.stop();
}

} // namespace rps::server
