#pragma once
#include <rps/engine/ScanEngine.hpp>
#include <rps/server/GuiSessionManager.hpp>
#include <rps/coordinator/Coordinator.hpp>
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
                               grpc::ServerWriter<rps::v1::PluginEvent>* writer) override;

    grpc::Status ClosePluginGui(grpc::ServerContext* context,
                                const rps::v1::ClosePluginGuiRequest* request,
                                rps::v1::ClosePluginGuiResponse* response) override;

    grpc::Status ShowPluginGui(grpc::ServerContext* context,
                               const rps::v1::ShowPluginGuiRequest* request,
                               rps::v1::ShowPluginGuiResponse* response) override;

    grpc::Status ClosePluginSession(grpc::ServerContext* context,
                                    const rps::v1::ClosePluginSessionRequest* request,
                                    rps::v1::ClosePluginSessionResponse* response) override;

    grpc::Status GetPluginState(grpc::ServerContext* context,
                                const rps::v1::GetPluginStateRequest* request,
                                rps::v1::GetPluginStateResponse* response) override;

    grpc::Status SetPluginState(grpc::ServerContext* context,
                                const rps::v1::SetPluginStateRequest* request,
                                rps::v1::SetPluginStateResponse* response) override;

    grpc::Status LoadPreset(grpc::ServerContext* context,
                            const rps::v1::LoadPresetRequest* request,
                            rps::v1::LoadPresetResponse* response) override;

    grpc::Status StreamAudio(grpc::ServerContext* context,
                             grpc::ServerReaderWriter<rps::v1::AudioOutputBlock,
                                                     rps::v1::AudioInputBlock>* stream) override;

    grpc::Status ListAudioDevices(grpc::ServerContext* context,
                                  const rps::v1::ListAudioDevicesRequest* request,
                                  rps::v1::ListAudioDevicesResponse* response) override;

    // -- Graph lifecycle (Phase 8) --

    grpc::Status CreateGraph(grpc::ServerContext* context,
                             const rps::v1::CreateGraphRequest* request,
                             rps::v1::CreateGraphResponse* response) override;

    grpc::Status DestroyGraph(grpc::ServerContext* context,
                              const rps::v1::DestroyGraphRequest* request,
                              rps::v1::DestroyGraphResponse* response) override;

    grpc::Status AddNode(grpc::ServerContext* context,
                         const rps::v1::AddNodeRequest* request,
                         rps::v1::AddNodeResponse* response) override;

    grpc::Status ConnectNodes(grpc::ServerContext* context,
                              const rps::v1::ConnectNodesRequest* request,
                              rps::v1::ConnectNodesResponse* response) override;

    grpc::Status ValidateGraph(grpc::ServerContext* context,
                               const rps::v1::ValidateGraphRequest* request,
                               rps::v1::ValidateGraphResponse* response) override;

    grpc::Status ActivateGraph(grpc::ServerContext* context,
                               const rps::v1::ActivateGraphRequest* request,
                               rps::v1::ActivateGraphResponse* response) override;

    grpc::Status DeactivateGraph(grpc::ServerContext* context,
                                 const rps::v1::DeactivateGraphRequest* request,
                                 rps::v1::DeactivateGraphResponse* response) override;

    grpc::Status GetGraphInfo(grpc::ServerContext* context,
                              const rps::v1::GetGraphInfoRequest* request,
                              rps::v1::GetGraphInfoResponse* response) override;

    // -- Chain convenience (Phase 8) --

    grpc::Status CreateChain(grpc::ServerContext* context,
                             const rps::v1::CreateChainRequest* request,
                             rps::v1::CreateChainResponse* response) override;

    grpc::Status DestroyChain(grpc::ServerContext* context,
                              const rps::v1::DestroyChainRequest* request,
                              rps::v1::DestroyChainResponse* response) override;

    // Called by main to set the server pointer for shutdown
    void setServer(grpc::Server* server);

    // Stop the current scan immediately (kills scanner children).
    // Safe to call from a signal handler context.
    void stopScan();

private:
    rps::engine::ScanEngine m_engine;
    GuiSessionManager m_guiManager;
    rps::coordinator::Coordinator m_coordinator;
    std::string m_dbPath;
    std::string m_scannerBin;
    std::chrono::steady_clock::time_point m_startTime;
    grpc::Server* m_server = nullptr;
    std::mutex m_serverMutex;
};

} // namespace rps::server
