#include <rps/server/RpsServiceImpl.hpp>
#include <rps/server/GrpcScanObserver.hpp>
#include <rps/engine/db/DatabaseManager.hpp>
#include <rps/audio/IAudioDevice.hpp>
#include <sqlite3.h>
#include <spdlog/spdlog.h>
#include <thread>
#include <atomic>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4100)
#endif
#include <boost/filesystem.hpp>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace rps::server {

RpsServiceImpl::RpsServiceImpl(const std::string& dbPath, const std::string& scannerBin)
    : m_guiManager(boost::filesystem::path(scannerBin).parent_path().string())
    , m_dbPath(dbPath)
    , m_scannerBin(scannerBin)
    , m_startTime(std::chrono::steady_clock::now()) {}

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
    m_guiManager.closeAll();
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

grpc::Status RpsServiceImpl::ListPlugins(grpc::ServerContext* /*context*/,
                                          const rps::v1::ListPluginsRequest* request,
                                          rps::v1::ListPluginsResponse* response) {
    try {
        rps::engine::db::DatabaseManager db(m_dbPath);
        db.initializeSchema();

        // Query all successfully scanned plugins, optionally filtered by format
        std::string sql = "SELECT id, format, path, name, uid, vendor, version, category, num_inputs, num_outputs "
                          "FROM plugins WHERE status = 'SUCCESS'";
        auto formatFilter = request->format_filter();
        if (!formatFilter.empty()) {
            sql += " AND format = ?";
        }
        sql += " ORDER BY name";

        sqlite3* rawDb = db.rawDb();
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(rawDb, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
            return grpc::Status(grpc::StatusCode::INTERNAL, "Failed to query plugins database");
        }

        if (!formatFilter.empty()) {
            sqlite3_bind_text(stmt, 1, formatFilter.c_str(), -1, SQLITE_TRANSIENT);
        }

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            auto* plugin = response->add_plugins();
            plugin->set_id(sqlite3_column_int64(stmt, 0));
            if (sqlite3_column_text(stmt, 1))
                plugin->set_format(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));
            if (sqlite3_column_text(stmt, 2))
                plugin->set_path(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2)));
            if (sqlite3_column_text(stmt, 3))
                plugin->set_name(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3)));
            if (sqlite3_column_text(stmt, 4))
                plugin->set_uid(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4)));
            if (sqlite3_column_text(stmt, 5))
                plugin->set_vendor(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5)));
            if (sqlite3_column_text(stmt, 6))
                plugin->set_version(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6)));
            if (sqlite3_column_text(stmt, 7))
                plugin->set_category(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7)));
            plugin->set_num_inputs(static_cast<uint32_t>(sqlite3_column_int(stmt, 8)));
            plugin->set_num_outputs(static_cast<uint32_t>(sqlite3_column_int(stmt, 9)));
        }
        sqlite3_finalize(stmt);

        spdlog::info("ListPlugins: returned {} plugins", response->plugins_size());
        return grpc::Status::OK;

    } catch (const std::exception& e) {
        return grpc::Status(grpc::StatusCode::INTERNAL,
                           std::string("Failed to list plugins: ") + e.what());
    }
}

grpc::Status RpsServiceImpl::OpenPluginGui(grpc::ServerContext* /*context*/,
                                            const rps::v1::OpenPluginGuiRequest* request,
                                            grpc::ServerWriter<rps::v1::PluginEvent>* writer) {
    auto pluginPath = request->plugin_path();
    auto format = request->format();

    if (pluginPath.empty() || format.empty()) {
        rps::v1::PluginEvent event;
        auto* err = event.mutable_gui_error();
        err->set_error("invalid_request");
        err->set_details("plugin_path and format are required");
        writer->Write(event);
        return grpc::Status::OK;
    }

    spdlog::info("OpenPluginGui: path={} format={} audio={}", pluginPath, format,
                 request->enable_audio());

    AudioConfig audioConfig;
    audioConfig.enabled = request->enable_audio();
    if (audioConfig.enabled) {
        audioConfig.sampleRate = request->sample_rate() > 0 ? request->sample_rate() : 48000;
        audioConfig.blockSize = request->block_size() > 0 ? request->block_size() : 128;
        audioConfig.numChannels = request->num_channels() > 0 ? request->num_channels() : 2;
        audioConfig.audioDevice = request->audio_device();
    }

    m_guiManager.openGui(pluginPath, format, writer, audioConfig);
    return grpc::Status::OK;
}

grpc::Status RpsServiceImpl::ClosePluginGui(grpc::ServerContext* /*context*/,
                                             const rps::v1::ClosePluginGuiRequest* request,
                                             rps::v1::ClosePluginGuiResponse* response) {
    auto pluginPath = request->plugin_path();
    bool wasOpen = m_guiManager.closeGui(pluginPath);
    response->set_was_open(wasOpen);
    spdlog::info("ClosePluginGui: path={} was_open={}", pluginPath, wasOpen);
    return grpc::Status::OK;
}

grpc::Status RpsServiceImpl::ShowPluginGui(grpc::ServerContext* /*context*/,
                                            const rps::v1::ShowPluginGuiRequest* request,
                                            rps::v1::ShowPluginGuiResponse* response) {
    auto pluginPath = request->plugin_path();
    bool ok = m_guiManager.showGui(pluginPath);
    response->set_success(ok);
    spdlog::info("ShowPluginGui: path={} success={}", pluginPath, ok);
    return grpc::Status::OK;
}

grpc::Status RpsServiceImpl::ClosePluginSession(grpc::ServerContext* /*context*/,
                                                 const rps::v1::ClosePluginSessionRequest* request,
                                                 rps::v1::ClosePluginSessionResponse* response) {
    auto pluginPath = request->plugin_path();
    bool wasOpen = m_guiManager.closeSession(pluginPath);
    response->set_was_open(wasOpen);
    spdlog::info("ClosePluginSession: path={} was_open={}", pluginPath, wasOpen);
    return grpc::Status::OK;
}

void RpsServiceImpl::stopScan() {
    m_engine.stop();
}

grpc::Status RpsServiceImpl::GetPluginState(grpc::ServerContext* /*context*/,
                                             const rps::v1::GetPluginStateRequest* request,
                                             rps::v1::GetPluginStateResponse* response) {
    auto pluginPath = request->plugin_path();
    spdlog::info("GetPluginState: path={}", pluginPath);

    auto result = m_guiManager.getState(pluginPath);
    response->set_state_data(result.state_data());
    response->set_success(result.success());
    if (!result.error().empty()) {
        response->set_error(result.error());
    }
    // TODO: set format from session info
    response->set_format("clap");

    spdlog::info("GetPluginState: success={} size={}", result.success(), result.state_data().size());
    return grpc::Status::OK;
}

grpc::Status RpsServiceImpl::SetPluginState(grpc::ServerContext* /*context*/,
                                             const rps::v1::SetPluginStateRequest* request,
                                             rps::v1::SetPluginStateResponse* response) {
    auto pluginPath = request->plugin_path();
    spdlog::info("SetPluginState: path={} size={}", pluginPath, request->state_data().size());

    auto result = m_guiManager.setState(pluginPath, request->state_data());
    response->set_success(result.success());
    if (!result.error().empty()) {
        response->set_error(result.error());
    }

    spdlog::info("SetPluginState: success={}", result.success());
    return grpc::Status::OK;
}

grpc::Status RpsServiceImpl::LoadPreset(grpc::ServerContext* /*context*/,
                                         const rps::v1::LoadPresetRequest* request,
                                         rps::v1::LoadPresetResponse* response) {
    auto pluginPath = request->plugin_path();
    auto presetId = request->preset_id();
    spdlog::info("LoadPreset: path={} preset_id={}", pluginPath, presetId);

    auto result = m_guiManager.loadPreset(pluginPath, presetId);
    response->set_success(result.success());
    if (!result.error().empty()) {
        response->set_error(result.error());
    }

    spdlog::info("LoadPreset: success={}", result.success());
    return grpc::Status::OK;
}

grpc::Status RpsServiceImpl::StreamAudio(
        grpc::ServerContext* /*context*/,
        grpc::ServerReaderWriter<rps::v1::AudioOutputBlock,
                                rps::v1::AudioInputBlock>* stream) {
    // Read the first block to determine which session to route to
    rps::v1::AudioInputBlock firstBlock;
    if (!stream->Read(&firstBlock)) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "No audio blocks received");
    }

    const auto& pluginPath = firstBlock.plugin_path();
    auto* ring = m_guiManager.getAudioRing(pluginPath);
    if (!ring) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND,
                            "No audio session for plugin: " + pluginPath);
    }

    spdlog::info("StreamAudio started for: {}", pluginPath);

    const uint32_t blockBytes = ring->blockSizeBytes();
    std::atomic<uint64_t> blocksWritten{0};
    std::atomic<bool> inputComplete{false};
    std::atomic<bool> clientDisconnected{false};

    // Reader thread: polls the output ring and sends AudioOutputBlock to client.
    // Keeps running until all expected output blocks have been sent.
    std::thread readerThread([&]() {
        uint64_t blocksSent = 0;
        std::vector<float> buf(ring->header().blockSize * ring->header().numChannels);

        while (!clientDisconnected.load(std::memory_order_relaxed)) {
            // Check if we've sent all expected output
            if (inputComplete.load(std::memory_order_relaxed) &&
                blocksSent >= blocksWritten.load(std::memory_order_relaxed)) {
                break;
            }

            if (ring->waitForOutput(std::chrono::milliseconds(50))) {
                if (ring->readOutputBlock(buf.data())) {
                    rps::v1::AudioOutputBlock outBlock;
                    outBlock.set_audio_data(buf.data(), buf.size() * sizeof(float));
                    outBlock.set_sequence(blocksSent);
                    if (!stream->Write(outBlock)) {
                        clientDisconnected.store(true, std::memory_order_relaxed);
                        break;
                    }
                    ++blocksSent;
                }
            }
        }

        spdlog::info("StreamAudio reader: sent {} output blocks", blocksSent);
    });

    // Process the first block
    uint64_t totalWritten = 0;
    if (firstBlock.audio_data().size() == blockBytes) {
        const auto* data = reinterpret_cast<const float*>(firstBlock.audio_data().data());
        while (!ring->writeInputBlock(data)) {
            std::this_thread::yield();
        }
        ++totalWritten;
        blocksWritten.store(totalWritten, std::memory_order_relaxed);
    }

    // Main loop: read input blocks from client, write to shared memory ring
    rps::v1::AudioInputBlock inBlock;
    while (stream->Read(&inBlock)) {
        if (inBlock.audio_data().size() != blockBytes) {
            spdlog::warn("StreamAudio: wrong block size {} (expected {})",
                         inBlock.audio_data().size(), blockBytes);
            continue;
        }
        const auto* data = reinterpret_cast<const float*>(inBlock.audio_data().data());

        // Spin-wait until input ring has space
        while (!ring->writeInputBlock(data)) {
            std::this_thread::yield();
        }
        ++totalWritten;
        blocksWritten.store(totalWritten, std::memory_order_relaxed);
    }

    // Signal that all input has been sent — reader thread will drain remaining output
    inputComplete.store(true, std::memory_order_relaxed);
    spdlog::info("StreamAudio: all {} input blocks written, waiting for output drain", totalWritten);

    readerThread.join();

    spdlog::info("StreamAudio ended for: {}", pluginPath);
    return grpc::Status::OK;
}

grpc::Status RpsServiceImpl::ListAudioDevices(
        grpc::ServerContext* /*context*/,
        const rps::v1::ListAudioDevicesRequest* request,
        rps::v1::ListAudioDevicesResponse* response) {
    const auto& filterBackend = request->backend();
    auto backends = rps::audio::availableAudioBackends();

    for (const auto& backend : backends) {
        if (!filterBackend.empty() && filterBackend != backend) {
            continue;
        }

        auto device = rps::audio::createAudioDevice(backend);
        if (!device) continue;

        for (const auto& info : device->enumerateDevices()) {
            auto* entry = response->add_devices();
            entry->set_backend(backend);
            entry->set_device_id(info.id);
            entry->set_name(info.name);
            entry->set_max_input_channels(info.maxInputChannels);
            entry->set_max_output_channels(info.maxOutputChannels);
            entry->set_is_default(info.isDefault);
        }
    }

    spdlog::info("ListAudioDevices: {} devices found", response->devices_size());
    return grpc::Status::OK;
}

} // namespace rps::server
