#pragma once

#include <rps/engine/ScanEngine.hpp>
#include <rps/server/GuiSessionManager.hpp>
#include <rps.grpc.pb.h>
#include <grpcpp/grpcpp.h>
#include <mutex>
#include <atomic>
#include <chrono>

namespace rps::server {

class RpsServiceImpl final : public rps::v1::RpsService::Service {
public:
    RpsServiceImpl(const std::string& dbPath, const std::string& scannerBin);

    grpc::Status StartScan(grpc::ServerContext* context,
                           const rps::v1::StartScanRequest* request,
                           grpc::ServerWriter<rps::v1::ScanEvent>* writer) override;

    grpc::Status StopScan(grpc::ServerContext* context,
                          const rps::v1::StopScanRequest* request,
                          rps::v1::StopScanResponse* response) override;

    grpc::Status GetStatus(grpc::ServerContext* context,
                           const rps::v1::GetStatusRequest* request,
                           rps::v1::GetStatusResponse* response) override;

    grpc::Status Shutdown(grpc::ServerContext* context,
                           const rps::v1::ShutdownRequest* request,
                           rps::v1::ShutdownResponse* response) override;

    grpc::Status ListPlugins(grpc::ServerContext* context,
                             const rps::v1::ListPluginsRequest* request,
                             rps::v1::ListPluginsResponse* response) override;

    grpc::Status OpenPluginGui(grpc::ServerContext* context,
                               const rps::v1::OpenPluginGuiRequest* request,
                               grpc::ServerWriter<rps::v1::PluginGuiEvent>* writer) override;

    grpc::Status ClosePluginGui(grpc::ServerContext* context,
                                const rps::v1::ClosePluginGuiRequest* request,
                                rps::v1::ClosePluginGuiResponse* response) override;

    // Called by main to set the server pointer for shutdown
    void setServer(grpc::Server* server);

    // Stop the current scan immediately (kills scanner children).
    // Safe to call from a signal handler context.
    void stopScan();

private:
    rps::engine::ScanEngine m_engine;
    GuiSessionManager m_guiManager;
    std::string m_dbPath;
    std::string m_scannerBin;
    std::chrono::steady_clock::time_point m_startTime;
    grpc::Server* m_server = nullptr;
    std::mutex m_serverMutex;
};

} // namespace rps::server
