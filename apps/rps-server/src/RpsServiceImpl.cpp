#include <rps/server/RpsServiceImpl.hpp>
#include <rps/server/GrpcScanObserver.hpp>
#include <rps/engine/db/DatabaseManager.hpp>
#include <rps/audio/IAudioDevice.hpp>
#include <rps/coordinator/GraphNode.hpp>
#include <rps/coordinator/AudioBuffer.hpp>
#include <rps/coordinator/ChannelFormat.hpp>
#include <SDL3/SDL.h>
#include <sqlite3.h>
#include <spdlog/spdlog.h>
#include <thread>
#include <atomic>
#include <format>

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
    , m_startTime(std::chrono::steady_clock::now()) {
    // Set the pluginhost binary path for multi-slice graph mode
    auto hostBin = boost::filesystem::path(scannerBin).parent_path() /
#ifdef _WIN32
        "rps-pluginhost.exe";
#else
        "rps-pluginhost";
#endif
    m_coordinator.setHostBinaryPath(hostBin.string());
}

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
    rps::v1::AudioInputBlock firstBlock;
    if (!stream->Read(&firstBlock)) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "No audio blocks received");
    }

    // ---------------------------------------------------------------------------
    // Graph mode: process blocks through GraphExecutor in-place
    // ---------------------------------------------------------------------------
    if (!firstBlock.graph_id().empty()) {
        const auto& graphId = firstBlock.graph_id();
        auto* executor = m_coordinator.getExecutor(graphId);
        if (!executor || !executor->isPrepared()) {
            return grpc::Status(grpc::StatusCode::NOT_FOUND,
                                "No active graph executor for: " + graphId);
        }

        // Determine block size and channels from the graph config
        auto* graph = m_coordinator.getGraph(graphId);
        if (!graph) {
            return grpc::Status(grpc::StatusCode::NOT_FOUND,
                                "Graph not found: " + graphId);
        }
        const auto& cfg = graph->config();
        const uint32_t numChannels = 2;  // chains are created with stereo
        const uint32_t blockFloats = cfg.blockSize * numChannels;
        const uint32_t blockBytes = blockFloats * sizeof(float);

        // Find input/output node IDs (chain convention)
        std::string inputNodeId = "chain_in";
        std::string outputNodeId = "chain_out";

        // --- Audio device for real-time playback (optional) ---
        constexpr uint32_t kPlaybackRingBlocks = 16;
        struct PlaybackRing {
            std::vector<float> buffer;
            std::atomic<uint32_t> writePos{0};
            std::atomic<uint32_t> readPos{0};
            uint32_t blockFloats = 0;
            uint32_t blockSize = 0;
            uint32_t numChannels = 0;
        };
        auto playbackRing = std::make_unique<PlaybackRing>();
        playbackRing->blockFloats = blockFloats;
        playbackRing->blockSize = cfg.blockSize;
        playbackRing->numChannels = numChannels;
        playbackRing->buffer.resize(kPlaybackRingBlocks * blockFloats, 0.0f);

        // Audio device callback — reads from playback ring
        auto audioCallback = [](const float* /*input*/, float* output,
                                uint32_t numFrames, void* userData) {
            auto* ring = static_cast<PlaybackRing*>(userData);
            if (!ring || !output) return;

            uint32_t rd = ring->readPos.load(std::memory_order_acquire);
            uint32_t wr = ring->writePos.load(std::memory_order_acquire);

            if (rd != wr) {
                uint32_t slot = rd % kPlaybackRingBlocks;
                const float* src = ring->buffer.data() + slot * ring->blockFloats;
                uint32_t framesToCopy = std::min(numFrames, ring->blockSize);
                std::memcpy(output, src, framesToCopy * ring->numChannels * sizeof(float));
                if (framesToCopy < numFrames) {
                    std::memset(output + framesToCopy * ring->numChannels, 0,
                                (numFrames - framesToCopy) * ring->numChannels * sizeof(float));
                }
                ring->readPos.store(rd + 1, std::memory_order_release);
            } else {
                std::memset(output, 0, numFrames * ring->numChannels * sizeof(float));
            }
        };

        std::unique_ptr<rps::audio::IAudioDevice> audioDevice;
        const auto& audioDeviceBackend = firstBlock.audio_device();
        if (!audioDeviceBackend.empty()) {
            // Ensure SDL3 audio subsystem is initialized (safe to call multiple times)
            if (!SDL_WasInit(SDL_INIT_AUDIO)) {
                if (!SDL_Init(SDL_INIT_AUDIO)) {
                    spdlog::error("SDL_Init(AUDIO) failed: {}", SDL_GetError());
                } else {
                    spdlog::info("SDL3 audio subsystem initialized");
                }
            }
            audioDevice = rps::audio::createAudioDevice(audioDeviceBackend);
            if (audioDevice) {
                rps::audio::AudioDeviceConfig devCfg;
                devCfg.sampleRate = cfg.sampleRate;
                devCfg.blockSize = cfg.blockSize;
                devCfg.numOutputChannels = numChannels;
                devCfg.numInputChannels = 0;

                if (audioDevice->open(devCfg, audioCallback, playbackRing.get())) {
                    audioDevice->start();
                    spdlog::info("StreamAudio(graph): opened {} audio device for playback",
                                 audioDeviceBackend);
                } else {
                    spdlog::warn("StreamAudio(graph): failed to open {} audio device",
                                 audioDeviceBackend);
                    audioDevice.reset();
                }
            }
        }

        spdlog::info("StreamAudio (graph mode) started for: {}", graphId);

        auto processOneBlock = [&](const rps::v1::AudioInputBlock& block) -> bool {
            if (block.audio_data().size() != blockBytes) {
                spdlog::warn("StreamAudio(graph): wrong block size {} (expected {})",
                             block.audio_data().size(), blockBytes);
                return true; // skip but continue
            }

            const auto* inData = reinterpret_cast<const float*>(block.audio_data().data());

            // Build deinterleaved input buffer
            coordinator::AudioBuffer inBuf(numChannels, cfg.blockSize);
            for (uint32_t ch = 0; ch < numChannels; ++ch) {
                auto* dst = inBuf.channel(ch);
                for (uint32_t s = 0; s < cfg.blockSize; ++s) {
                    dst[s] = inData[s * numChannels + ch];
                }
            }

            // Process
            std::unordered_map<std::string, coordinator::AudioBuffer> inputBuffers;
            inputBuffers.emplace(inputNodeId, std::move(inBuf));

            std::unordered_map<std::string, coordinator::AudioBuffer> outputBuffers;
            outputBuffers.emplace(outputNodeId, coordinator::AudioBuffer(numChannels, cfg.blockSize));

            try {
                executor->processBlock(inputBuffers, outputBuffers);
            } catch (const std::exception& e) {
                spdlog::error("StreamAudio(graph): processBlock failed: {}", e.what());
                return false;
            }

            // Re-interleave output
            auto& outBuf = outputBuffers[outputNodeId];
            std::vector<float> outInterleaved(blockFloats);
            for (uint32_t ch = 0; ch < numChannels; ++ch) {
                const auto* src = outBuf.channel(ch);
                for (uint32_t s = 0; s < cfg.blockSize; ++s) {
                    outInterleaved[s * numChannels + ch] = src[s];
                }
            }

            // Write to playback ring for SDL audio device
            if (audioDevice) {
                uint32_t wr = playbackRing->writePos.load(std::memory_order_relaxed);
                uint32_t rd = playbackRing->readPos.load(std::memory_order_acquire);
                if (wr - rd < kPlaybackRingBlocks) {
                    uint32_t slot = wr % kPlaybackRingBlocks;
                    std::memcpy(
                        playbackRing->buffer.data() + slot * blockFloats,
                        outInterleaved.data(),
                        blockFloats * sizeof(float));
                    playbackRing->writePos.store(wr + 1, std::memory_order_release);
                }
                // else: playback ring full, skip (SDL will under-run gracefully)
            }

            rps::v1::AudioOutputBlock outBlock;
            outBlock.set_audio_data(outInterleaved.data(), outInterleaved.size() * sizeof(float));
            outBlock.set_sequence(block.sequence());
            return stream->Write(outBlock);
        };

        // Process first block
        if (!processOneBlock(firstBlock)) {
            if (audioDevice) { audioDevice->stop(); audioDevice->close(); }
            return grpc::Status::OK;
        }

        // Process remaining blocks
        rps::v1::AudioInputBlock inBlock;
        uint64_t totalBlocks = 1;
        while (stream->Read(&inBlock)) {
            if (!processOneBlock(inBlock)) break;
            ++totalBlocks;
        }

        // Clean up audio device
        if (audioDevice) {
            // Let playback ring drain before stopping
            for (int i = 0; i < 50; ++i) {
                uint32_t rd = playbackRing->readPos.load(std::memory_order_acquire);
                uint32_t wr = playbackRing->writePos.load(std::memory_order_acquire);
                if (rd >= wr) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            audioDevice->stop();
            audioDevice->close();
            spdlog::info("StreamAudio(graph): audio device closed");
        }

        spdlog::info("StreamAudio (graph mode) ended for: {} ({} blocks)", graphId, totalBlocks);
        return grpc::Status::OK;
    }

    // ---------------------------------------------------------------------------
    // Plugin mode: route through GUI session's audio ring (existing behavior)
    // ---------------------------------------------------------------------------
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

    std::thread readerThread([&]() {
        uint64_t blocksSent = 0;
        std::vector<float> buf(ring->header().blockSize * ring->header().numChannels);

        while (!clientDisconnected.load(std::memory_order_relaxed)) {
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

    uint64_t totalWritten = 0;
    if (firstBlock.audio_data().size() == blockBytes) {
        const auto* data = reinterpret_cast<const float*>(firstBlock.audio_data().data());
        while (!ring->writeInputBlock(data)) {
            std::this_thread::yield();
        }
        ++totalWritten;
        blocksWritten.store(totalWritten, std::memory_order_relaxed);
    }

    rps::v1::AudioInputBlock inBlock;
    while (stream->Read(&inBlock)) {
        if (inBlock.audio_data().size() != blockBytes) {
            spdlog::warn("StreamAudio: wrong block size {} (expected {})",
                         inBlock.audio_data().size(), blockBytes);
            continue;
        }
        const auto* data = reinterpret_cast<const float*>(inBlock.audio_data().data());

        while (!ring->writeInputBlock(data)) {
            std::this_thread::yield();
        }
        ++totalWritten;
        blocksWritten.store(totalWritten, std::memory_order_relaxed);
    }

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

// ---------------------------------------------------------------------------
// Graph API handlers (Phase 8)
// ---------------------------------------------------------------------------

namespace {

/// Convert a proto ChannelLayoutMsg to coordinator::ChannelLayout.
coordinator::ChannelLayout protoToLayout(const rps::v1::ChannelLayoutMsg& msg) {
    coordinator::ChannelLayout layout;
    if (!msg.format().empty()) {
        layout.format = coordinator::channelFormatFromString(msg.format());
    }
    layout.channelCount = msg.channel_count();
    uint32_t named = coordinator::getChannelCount(layout.format);
    if (named > 0 && layout.channelCount == 0) {
        layout.channelCount = named;
    }
    return layout;
}

/// Convert a proto GraphNodeMsg to a coordinator::GraphNode.
coordinator::GraphNode protoNodeToGraphNode(const rps::v1::GraphNodeMsg& msg) {
    auto typeOpt = coordinator::nodeTypeFromString(msg.type());
    if (!typeOpt) {
        throw std::runtime_error(std::format("Unknown node type: '{}'", msg.type()));
    }

    coordinator::GraphNode node;
    switch (*typeOpt) {
        case coordinator::NodeType::Input: {
            coordinator::InputNodeConfig c;
            if (msg.has_io_layout()) c.layout = protoToLayout(msg.io_layout());
            c.shmName = msg.shm_name();
            node = coordinator::createInputNode(msg.id(), c);
            break;
        }
        case coordinator::NodeType::Output: {
            coordinator::OutputNodeConfig c;
            if (msg.has_io_layout()) c.layout = protoToLayout(msg.io_layout());
            c.shmName = msg.shm_name();
            node = coordinator::createOutputNode(msg.id(), c);
            break;
        }
        case coordinator::NodeType::Plugin: {
            coordinator::PluginNodeConfig c;
            c.pluginPath = msg.plugin_path();
            c.format = msg.plugin_format();
            coordinator::ChannelLayout ioLayout;
            if (msg.has_plugin_io_layout()) {
                ioLayout = protoToLayout(msg.plugin_io_layout());
            } else {
                ioLayout = {coordinator::ChannelFormat::Stereo, 2};
            }
            node = coordinator::createPluginNode(msg.id(), c, ioLayout);
            break;
        }
        case coordinator::NodeType::Gain: {
            coordinator::GainNodeConfig c;
            if (msg.has_gain_layout()) c.layout = protoToLayout(msg.gain_layout());
            c.gain = msg.gain_value();
            c.mute = msg.gain_mute();
            c.bypass = msg.gain_bypass();
            node = coordinator::createGainNode(msg.id(), c);
            break;
        }
        case coordinator::NodeType::Mixer: {
            coordinator::MixerNodeConfig c;
            if (msg.has_mixer_output_layout()) c.outputLayout = protoToLayout(msg.mixer_output_layout());
            c.numInputs = msg.mixer_num_inputs();
            for (float g : msg.mixer_input_gains()) {
                c.inputGains.push_back(g);
            }
            node = coordinator::createMixerNode(msg.id(), c);
            break;
        }
        default:
            throw std::runtime_error(std::format("Node type '{}' not supported via gRPC yet", msg.type()));
    }

    node.latencySamples = msg.latency_samples();
    node.sliceHint = msg.slice_hint();
    return node;
}

} // anonymous namespace

grpc::Status RpsServiceImpl::CreateGraph(grpc::ServerContext* /*context*/,
                                          const rps::v1::CreateGraphRequest* request,
                                          rps::v1::CreateGraphResponse* response) {
    try {
        uint32_t sampleRate = request->sample_rate() > 0 ? request->sample_rate() : 48000;
        uint32_t blockSize = request->block_size() > 0 ? request->block_size() : 128;

        auto graphId = m_coordinator.createGraph({sampleRate, blockSize});
        response->set_graph_id(graphId);
        spdlog::info("CreateGraph: id={} sr={} bs={}", graphId, sampleRate, blockSize);
        return grpc::Status::OK;
    } catch (const std::exception& e) {
        return grpc::Status(grpc::StatusCode::INTERNAL, e.what());
    }
}

grpc::Status RpsServiceImpl::DestroyGraph(grpc::ServerContext* /*context*/,
                                           const rps::v1::DestroyGraphRequest* request,
                                           rps::v1::DestroyGraphResponse* /*response*/) {
    try {
        m_coordinator.destroyGraph(request->graph_id());
        spdlog::info("DestroyGraph: id={}", request->graph_id());
        return grpc::Status::OK;
    } catch (const std::exception& e) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND, e.what());
    }
}

grpc::Status RpsServiceImpl::AddNode(grpc::ServerContext* /*context*/,
                                      const rps::v1::AddNodeRequest* request,
                                      rps::v1::AddNodeResponse* response) {
    try {
        auto node = protoNodeToGraphNode(request->node());
        auto nodeId = m_coordinator.addNode(request->graph_id(), std::move(node));
        response->set_node_id(nodeId);
        spdlog::info("AddNode: graph={} node={}", request->graph_id(), nodeId);
        return grpc::Status::OK;
    } catch (const std::exception& e) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, e.what());
    }
}

grpc::Status RpsServiceImpl::ConnectNodes(grpc::ServerContext* /*context*/,
                                           const rps::v1::ConnectNodesRequest* request,
                                           rps::v1::ConnectNodesResponse* response) {
    try {
        auto edgeId = m_coordinator.connectNodes(
            request->graph_id(),
            request->source_node_id(), request->source_port(),
            request->dest_node_id(), request->dest_port());
        response->set_edge_id(edgeId);
        spdlog::info("ConnectNodes: graph={} {}:{} -> {}:{}",
                     request->graph_id(),
                     request->source_node_id(), request->source_port(),
                     request->dest_node_id(), request->dest_port());
        return grpc::Status::OK;
    } catch (const std::exception& e) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, e.what());
    }
}

grpc::Status RpsServiceImpl::ValidateGraph(grpc::ServerContext* /*context*/,
                                            const rps::v1::ValidateGraphRequest* request,
                                            rps::v1::ValidateGraphResponse* response) {
    try {
        auto result = m_coordinator.validateGraph(request->graph_id());
        response->set_valid(result.valid);
        for (const auto& err : result.errors) {
            response->add_errors(err.message);
        }
        spdlog::info("ValidateGraph: graph={} valid={}", request->graph_id(), result.valid);
        return grpc::Status::OK;
    } catch (const std::exception& e) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND, e.what());
    }
}

grpc::Status RpsServiceImpl::ActivateGraph(grpc::ServerContext* /*context*/,
                                            const rps::v1::ActivateGraphRequest* request,
                                            rps::v1::ActivateGraphResponse* response) {
    try {
        auto strategyStr = request->strategy();
        coordinator::SlicingStrategy strategy = coordinator::SlicingStrategy::Performance;
        if (strategyStr == "default") {
            strategy = coordinator::SlicingStrategy::Default;
        } else if (strategyStr == "crash_isolation") {
            strategy = coordinator::SlicingStrategy::CrashIsolation;
        }

        m_coordinator.activateGraph(request->graph_id(), strategy);
        auto info = m_coordinator.getGraphInfo(request->graph_id());
        response->set_success(true);
        response->set_slice_count(info.sliceCount);
        spdlog::info("ActivateGraph: graph={} strategy={} slices={}",
                     request->graph_id(), strategyStr.empty() ? "performance" : strategyStr,
                     info.sliceCount);
        return grpc::Status::OK;
    } catch (const std::exception& e) {
        response->set_success(false);
        response->set_error(e.what());
        return grpc::Status::OK;
    }
}

grpc::Status RpsServiceImpl::DeactivateGraph(grpc::ServerContext* /*context*/,
                                              const rps::v1::DeactivateGraphRequest* request,
                                              rps::v1::DeactivateGraphResponse* /*response*/) {
    try {
        m_coordinator.deactivateGraph(request->graph_id());
        spdlog::info("DeactivateGraph: graph={}", request->graph_id());
        return grpc::Status::OK;
    } catch (const std::exception& e) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND, e.what());
    }
}

grpc::Status RpsServiceImpl::GetGraphInfo(grpc::ServerContext* /*context*/,
                                           const rps::v1::GetGraphInfoRequest* request,
                                           rps::v1::GetGraphInfoResponse* response) {
    try {
        auto info = m_coordinator.getGraphInfo(request->graph_id());
        response->set_graph_id(info.graphId);
        response->set_name(info.name);
        response->set_state(info.state == coordinator::GraphState::Active ? "active" : "inactive");
        response->set_node_count(info.nodeCount);
        response->set_edge_count(info.edgeCount);
        response->set_slice_count(info.sliceCount);
        switch (info.strategy) {
            case coordinator::SlicingStrategy::Performance: response->set_strategy("performance"); break;
            case coordinator::SlicingStrategy::Default: response->set_strategy("default"); break;
            case coordinator::SlicingStrategy::CrashIsolation: response->set_strategy("crash_isolation"); break;
        }
        return grpc::Status::OK;
    } catch (const std::exception& e) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND, e.what());
    }
}

// ---------------------------------------------------------------------------
// Chain API handlers (Phase 8)
// ---------------------------------------------------------------------------

grpc::Status RpsServiceImpl::CreateChain(grpc::ServerContext* /*context*/,
                                          const rps::v1::CreateChainRequest* request,
                                          rps::v1::CreateChainResponse* response) {
    try {
        if (request->plugins_size() == 0) {
            response->set_success(false);
            response->set_error("At least one plugin is required");
            return grpc::Status::OK;
        }

        uint32_t sampleRate = request->sample_rate() > 0 ? request->sample_rate() : 48000;
        uint32_t blockSize = request->block_size() > 0 ? request->block_size() : 128;
        uint32_t numChannels = request->num_channels() > 0 ? request->num_channels() : 2;

        coordinator::ChannelFormat chanFmt = coordinator::ChannelFormat::Stereo;
        if (numChannels == 1) chanFmt = coordinator::ChannelFormat::Mono;
        coordinator::ChannelLayout layout{chanFmt, numChannels};

        auto graphId = m_coordinator.createGraph({sampleRate, blockSize});

        // Store user-provided name if any
        if (!request->name().empty()) {
            m_coordinator.setGraphName(graphId, request->name());
        }

        // Add input node
        m_coordinator.addNode(graphId, coordinator::createInputNode("chain_in", {layout, ""}));

        // Add plugin nodes
        std::vector<std::string> nodeIds;
        nodeIds.push_back("chain_in");
        for (int i = 0; i < request->plugins_size(); ++i) {
            const auto& entry = request->plugins(i);
            std::string nodeId = std::format("plugin_{}", i);
            coordinator::PluginNodeConfig pc;
            pc.pluginPath = entry.plugin_path();
            pc.format = entry.format();
            m_coordinator.addNode(graphId, coordinator::createPluginNode(nodeId, pc, layout));
            nodeIds.push_back(nodeId);
        }

        // Add output node
        m_coordinator.addNode(graphId, coordinator::createOutputNode("chain_out", {layout, ""}));
        nodeIds.push_back("chain_out");

        // Connect linearly: in -> plugin_0 -> plugin_1 -> ... -> out
        for (size_t i = 0; i + 1 < nodeIds.size(); ++i) {
            m_coordinator.connectNodes(graphId, nodeIds[i], 0, nodeIds[i + 1], 0);
        }

        // Activate with Performance strategy (single process)
        m_coordinator.activateGraph(graphId, coordinator::SlicingStrategy::Performance);

        response->set_graph_id(graphId);
        response->set_success(true);
        spdlog::info("CreateChain: graph={} plugins={} sr={} bs={} ch={}",
                     graphId, request->plugins_size(), sampleRate, blockSize, numChannels);
        return grpc::Status::OK;

    } catch (const std::exception& e) {
        response->set_success(false);
        response->set_error(e.what());
        return grpc::Status::OK;
    }
}

grpc::Status RpsServiceImpl::DestroyChain(grpc::ServerContext* /*context*/,
                                           const rps::v1::DestroyChainRequest* request,
                                           rps::v1::DestroyChainResponse* /*response*/) {
    try {
        m_coordinator.destroyGraph(request->graph_id());
        spdlog::info("DestroyChain: graph={}", request->graph_id());
        return grpc::Status::OK;
    } catch (const std::exception& e) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND, e.what());
    }
}

// ---------------------------------------------------------------------------
// Phase 11: Graph detail & mutation handlers
// ---------------------------------------------------------------------------

grpc::Status RpsServiceImpl::GetGraphDetail(grpc::ServerContext* /*context*/,
                                             const rps::v1::GetGraphDetailRequest* request,
                                             rps::v1::GetGraphDetailResponse* response) {
    try {
        auto info = m_coordinator.getGraphInfo(request->graph_id());
        auto* graph = m_coordinator.getGraph(request->graph_id());
        if (!graph) {
            return grpc::Status(grpc::StatusCode::NOT_FOUND,
                                std::format("Graph '{}' not found", request->graph_id()));
        }

        response->set_graph_id(info.graphId);
        response->set_name(info.name);
        response->set_state(info.state == coordinator::GraphState::Active ? "active" : "inactive");
        switch (info.strategy) {
            case coordinator::SlicingStrategy::Performance: response->set_strategy("performance"); break;
            case coordinator::SlicingStrategy::Default: response->set_strategy("default"); break;
            case coordinator::SlicingStrategy::CrashIsolation: response->set_strategy("crash_isolation"); break;
        }

        // Populate nodes
        for (const auto& [nodeId, node] : graph->nodes()) {
            auto* nd = response->add_nodes();
            nd->set_node_id(nodeId);
            nd->set_type(std::string(coordinator::nodeTypeToString(node.type)));
            if (node.pluginConfig) {
                nd->set_plugin_path(node.pluginConfig->pluginPath);
                nd->set_format(node.pluginConfig->format);
            }
            nd->set_input_channels(static_cast<uint32_t>(node.inputPorts.size() > 0 ? node.inputPorts[0].layout.channelCount : 0));
            nd->set_output_channels(static_cast<uint32_t>(node.outputPorts.size() > 0 ? node.outputPorts[0].layout.channelCount : 0));
            nd->set_input_port_count(static_cast<uint32_t>(node.inputPorts.size()));
            nd->set_output_port_count(static_cast<uint32_t>(node.outputPorts.size()));
        }

        // Populate edges
        for (const auto& edge : graph->edges()) {
            auto* ed = response->add_edges();
            ed->set_edge_id(edge.id);
            ed->set_source_node_id(edge.sourceNodeId);
            ed->set_source_port(edge.sourcePort);
            ed->set_dest_node_id(edge.destNodeId);
            ed->set_dest_port(edge.destPort);
        }

        spdlog::debug("GetGraphDetail: graph={} nodes={} edges={}",
                       request->graph_id(), graph->nodeCount(), graph->edges().size());
        return grpc::Status::OK;
    } catch (const std::exception& e) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND, e.what());
    }
}

grpc::Status RpsServiceImpl::RemoveNode(grpc::ServerContext* /*context*/,
                                         const rps::v1::RemoveNodeRequest* request,
                                         rps::v1::RemoveNodeResponse* /*response*/) {
    try {
        m_coordinator.removeNode(request->graph_id(), request->node_id());
        spdlog::info("RemoveNode: graph={} node={}", request->graph_id(), request->node_id());
        return grpc::Status::OK;
    } catch (const std::exception& e) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND, e.what());
    }
}

grpc::Status RpsServiceImpl::DisconnectNodes(grpc::ServerContext* /*context*/,
                                              const rps::v1::DisconnectNodesRequest* request,
                                              rps::v1::DisconnectNodesResponse* /*response*/) {
    try {
        m_coordinator.disconnectNodes(request->graph_id(), request->edge_id());
        spdlog::info("DisconnectNodes: graph={} edge={}", request->graph_id(), request->edge_id());
        return grpc::Status::OK;
    } catch (const std::exception& e) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND, e.what());
    }
}

grpc::Status RpsServiceImpl::OpenGraphNodeGui(grpc::ServerContext* /*context*/,
                                               const rps::v1::OpenGraphNodeGuiRequest* request,
                                               grpc::ServerWriter<rps::v1::PluginEvent>* /*writer*/) {
    // TODO: Implement per-node GUI hosting within graph mode
    spdlog::warn("OpenGraphNodeGui: not yet implemented (graph={} node={})",
                 request->graph_id(), request->node_id());
    return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "Per-node GUI not yet implemented");
}

grpc::Status RpsServiceImpl::CloseGraphNodeGui(grpc::ServerContext* /*context*/,
                                                const rps::v1::CloseGraphNodeGuiRequest* request,
                                                rps::v1::CloseGraphNodeGuiResponse* /*response*/) {
    // TODO: Implement per-node GUI close within graph mode
    spdlog::warn("CloseGraphNodeGui: not yet implemented (graph={} node={})",
                 request->graph_id(), request->node_id());
    return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "Per-node GUI not yet implemented");
}

} // namespace rps::server

