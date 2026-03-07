#include <rps/server/GuiSessionManager.hpp>
#include <rps/ipc/Messages.hpp>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4100)
#pragma warning(disable: 4244)
#pragma warning(disable: 4245)
#endif
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <boost/process/v1/child.hpp>
#include <boost/process/v1/io.hpp>
#include <boost/json.hpp>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <rps.grpc.pb.h>
#include <grpcpp/grpcpp.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <thread>
#include <future> // Added for promise/future

namespace bp = boost::process::v1;

namespace rps::server {

GuiSessionManager::GuiSessionManager(const std::string& hostBinDir)
    : m_hostBinDir(hostBinDir) {}

boost::filesystem::path GuiSessionManager::resolveHostBinary(const std::string& format) const {
    std::string binaryName;
    if (format == "clap") {
        binaryName = "rps-pluginhost-clap";
    } else if (format == "vst3") {
        binaryName = "rps-pluginhost-vst3";
    } else {
        // Future: "vst2" -> "rps-pluginhost-vst2", etc.
        return {};
    }

#ifdef _WIN32
    binaryName += ".exe";
#endif

    // Check in the host binary directory (typically next to rps-server)
    boost::filesystem::path candidate = boost::filesystem::path(m_hostBinDir) / binaryName;
    if (boost::filesystem::exists(candidate)) {
        return candidate;
    }

    // Fall back to current directory
    candidate = boost::filesystem::path(binaryName);
    if (boost::filesystem::exists(candidate)) {
        return candidate;
    }

    return {};
}

void GuiSessionManager::openGui(const std::string& pluginPath, const std::string& format,
                                 grpc::ServerWriter<rps::v1::PluginEvent>* writer,
                                 const AudioConfig& audioConfig) {
    // Check if already open
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_sessions.count(pluginPath)) {
            rps::v1::PluginEvent event;
            auto* err = event.mutable_gui_error();
            err->set_error("already_open");
            err->set_details("Plugin GUI is already open for: " + pluginPath);
            writer->Write(event);
            return;
        }
    }

    // Resolve host binary
    auto hostBin = resolveHostBinary(format);
    if (hostBin.empty()) {
        rps::v1::PluginEvent event;
        auto* err = event.mutable_gui_error();
        err->set_error("unsupported_format");
        err->set_details("GUI hosting is not supported for format: " + format);
        writer->Write(event);
        return;
    }

    // Generate IPC ID
    auto ipcId = "rps_gui_" + boost::uuids::to_string(boost::uuids::random_generator()());

    // Create IPC connection (server side)
    auto connection = rps::ipc::MessageQueueConnection::createServer(ipcId);
    if (!connection) {
        rps::v1::PluginEvent event;
        auto* err = event.mutable_gui_error();
        err->set_error("ipc_failed");
        err->set_details("Failed to create IPC message queue.");
        writer->Write(event);
        return;
    }

    spdlog::info("Launching plugin host: {} for {} (ipc: {})", hostBin.string(), pluginPath, ipcId);

    // Create shared memory for audio if requested
    std::unique_ptr<rps::audio::SharedAudioRing> audioRing;
    std::string shmName;
    if (audioConfig.enabled) {
        shmName = "rps_audio_" + boost::uuids::to_string(boost::uuids::random_generator()());
        try {
            audioRing = rps::audio::SharedAudioRing::create(
                shmName, audioConfig.sampleRate, audioConfig.blockSize,
                audioConfig.numChannels);
            spdlog::info("Audio shared memory created: {}", shmName);
        } catch (const std::exception& e) {
            spdlog::error("Failed to create audio shared memory: {}", e.what());
            shmName.clear();
        }
    }

    // Spawn the host process
    std::unique_ptr<bp::child> child;
    try {
        if (!shmName.empty()) {
            child = std::make_unique<bp::child>(
                hostBin.string(),
                "--ipc-id", ipcId,
                "--plugin-path", pluginPath,
                "--format", format,
                "--audio-shm", shmName
            );
        } else {
            child = std::make_unique<bp::child>(
                hostBin.string(),
                "--ipc-id", ipcId,
                "--plugin-path", pluginPath,
                "--format", format
            );
        }
    } catch (const std::exception& e) {
        rps::v1::PluginEvent event;
        auto* err = event.mutable_gui_error();
        err->set_error("spawn_failed");
        err->set_details(std::string("Failed to spawn host process: ") + e.what());
        writer->Write(event);
        return;
    }

    // Register the session
    auto session = std::make_unique<Session>();
    session->pluginPath = pluginPath;
    session->ipcId = ipcId;
    session->shmName = shmName;
    session->audioRing = std::move(audioRing);
    session->connection = std::move(connection);
    session->process = std::move(child);

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_sessions[pluginPath] = std::move(session);
    }

    // Wait for events from the worker via IPC and forward to gRPC
    bool done = false;
    while (!done) {
        Session* currentSession = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_sessions.find(pluginPath);
            if (it == m_sessions.end()) break;
            currentSession = it->second.get();
        }
        if (!currentSession) break;

        auto msg = currentSession->connection->receiveMessage(500);
        if (!msg) {
            // Check if process is still running
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_sessions.find(pluginPath);
            if (it == m_sessions.end()) break;
            if (!it->second->process->running()) {
                // Process exited unexpectedly
                rps::v1::PluginEvent event;
                auto* closed = event.mutable_gui_closed();
                closed->set_reason("crash");
                writer->Write(event);
                done = true;
            }
            continue;
        }

        switch (msg->type) {
            case rps::ipc::MessageType::GuiOpenedEvent: {
                auto& evt = std::get<rps::ipc::GuiOpenedEvent>(msg->payload);
                rps::v1::PluginEvent event;
                auto* opened = event.mutable_gui_opened();
                opened->set_plugin_name(evt.pluginName);
                opened->set_width(evt.width);
                opened->set_height(evt.height);
                writer->Write(event);
                spdlog::info("Plugin GUI opened: {} ({}x{})", evt.pluginName, evt.width, evt.height);

                // Send AudioReady if audio shared memory was created
                if (!currentSession->shmName.empty() && currentSession->audioRing) {
                    rps::v1::PluginEvent audioEvent;
                    auto* ready = audioEvent.mutable_audio_ready();
                    ready->set_shm_name(currentSession->shmName);
                    const auto& hdr = currentSession->audioRing->header();
                    ready->set_sample_rate(hdr.sampleRate);
                    ready->set_block_size(hdr.blockSize);
                    ready->set_num_channels(hdr.numChannels);
                    ready->set_ring_blocks(hdr.ringBlocks);
                    ready->set_latency_samples(hdr.latencySamples);
                    writer->Write(audioEvent);
                    spdlog::info("AudioReady sent: shm={}", currentSession->shmName);
                }
                break;
            }
            case rps::ipc::MessageType::GuiClosedEvent: {
                auto& evt = std::get<rps::ipc::GuiClosedEvent>(msg->payload);
                rps::v1::PluginEvent event;
                auto* closed = event.mutable_gui_closed();
                closed->set_reason(evt.reason);
                writer->Write(event);
                spdlog::info("Plugin GUI closed: {} (reason: {})", pluginPath, evt.reason);
                done = true;
                break;
            }
            case rps::ipc::MessageType::ParameterListEvent: {
                auto& evt = std::get<rps::ipc::ParameterListEvent>(msg->payload);
                rps::v1::PluginEvent event;
                auto* paramList = event.mutable_parameter_list();
                for (const auto& p : evt.parameters) {
                    auto* pp = paramList->add_parameters();
                    pp->set_id(p.id);
                    pp->set_index(p.index);
                    pp->set_name(p.name);
                    pp->set_module(p.module);
                    pp->set_min_value(p.minValue);
                    pp->set_max_value(p.maxValue);
                    pp->set_default_value(p.defaultValue);
                    pp->set_current_value(p.currentValue);
                    pp->set_display_text(p.displayText);
                    pp->set_flags(p.flags);
                }
                writer->Write(event);
                spdlog::info("Sent ParameterList ({} params)", evt.parameters.size());
                break;
            }
            case rps::ipc::MessageType::ParameterValuesEvent: {
                auto& evt = std::get<rps::ipc::ParameterValuesEvent>(msg->payload);
                rps::v1::PluginEvent event;
                auto* updates = event.mutable_parameter_updates();
                for (const auto& u : evt.updates) {
                    auto* pu = updates->add_updates();
                    pu->set_param_id(u.paramId);
                    pu->set_value(u.value);
                    pu->set_display_text(u.displayText);
                }
                writer->Write(event);
                break;
            }
            case rps::ipc::MessageType::GetStateResponse: {
                auto& resp = std::get<rps::ipc::GetStateResponse>(msg->payload);
                std::lock_guard<std::mutex> slock(currentSession->stateMutex);
                if (currentSession->pendingGetState) {
                    currentSession->pendingGetState->set_value(std::move(resp));
                    currentSession->pendingGetState.reset();
                    spdlog::info("Fulfilled pendingGetState");
                }
                break;
            }
            case rps::ipc::MessageType::SetStateResponse: {
                auto& resp = std::get<rps::ipc::SetStateResponse>(msg->payload);
                std::lock_guard<std::mutex> slock(currentSession->stateMutex);
                if (currentSession->pendingSetState) {
                    currentSession->pendingSetState->set_value(std::move(resp));
                    currentSession->pendingSetState.reset();
                    spdlog::info("Fulfilled pendingSetState");
                }
                break;
            }
            case rps::ipc::MessageType::PresetListEvent: {
                auto& evt = std::get<rps::ipc::PresetListEvent>(msg->payload);
                rps::v1::PluginEvent event;
                auto* presetList = event.mutable_preset_list();
                for (const auto& p : evt.presets) {
                    auto* pp = presetList->add_presets();
                    pp->set_id(p.id);
                    pp->set_name(p.name);
                    pp->set_category(p.category);
                    pp->set_creator(p.creator);
                    pp->set_index(p.index);
                    pp->set_flags(p.flags);
                }
                writer->Write(event);
                spdlog::info("Sent PresetList ({} presets)", evt.presets.size());
                break;
            }
            case rps::ipc::MessageType::LoadPresetResponse: {
                auto& resp = std::get<rps::ipc::LoadPresetResponse>(msg->payload);
                std::lock_guard<std::mutex> slock(currentSession->stateMutex);
                if (currentSession->pendingLoadPreset) {
                    currentSession->pendingLoadPreset->set_value(std::move(resp));
                    currentSession->pendingLoadPreset.reset();
                    spdlog::info("Fulfilled pendingLoadPreset");
                }
                break;
            }
            default:
                break;
        }
    }

    // Clean up session
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_sessions.find(pluginPath);
        if (it != m_sessions.end()) {
            if (it->second->process && it->second->process->running()) {
                it->second->process->terminate();
                it->second->process->wait();
            }
            m_sessions.erase(it);
        }
    }
}

bool GuiSessionManager::closeGui(const std::string& pluginPath) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_sessions.find(pluginPath);
    if (it == m_sessions.end()) {
        return false;
    }

    // Send CloseGuiRequest via IPC
    rps::ipc::Message msg;
    msg.type = rps::ipc::MessageType::CloseGuiRequest;
    msg.payload = rps::ipc::CloseGuiRequest{};
    it->second->connection->sendMessage(msg);

    return true;
}

void GuiSessionManager::closeAll() {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& [path, session] : m_sessions) {
        // Send CloseGuiRequest
        rps::ipc::Message msg;
        msg.type = rps::ipc::MessageType::CloseGuiRequest;
        msg.payload = rps::ipc::CloseGuiRequest{};
        try {
            session->connection->sendMessage(msg);
        } catch (...) {}

        // Give it a moment then force kill
        if (session->process && session->process->running()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            if (session->process->running()) {
                session->process->terminate();
            }
            session->process->wait();
        }
    }
    m_sessions.clear();
}

rps::ipc::GetStateResponse GuiSessionManager::getState(const std::string& pluginPath) {
    std::future<rps::ipc::GetStateResponse> future;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_sessions.find(pluginPath);
        if (it == m_sessions.end()) {
            return {{}, false, "No active GUI session for: " + pluginPath};
        }
        auto* session = it->second.get();

        // Install promise and get future (under session lock)
        {
            std::lock_guard<std::mutex> slock(session->stateMutex);
            session->pendingGetState.emplace();
            future = session->pendingGetState->get_future();
        }

        // Send GetStateRequest
        rps::ipc::Message req;
        req.type = rps::ipc::MessageType::GetStateRequest;
        req.payload = rps::ipc::GetStateRequest{};
        session->connection->sendMessage(req);
    }

    // Wait for response (unlocked — relay loop will fulfill the promise)
    if (future.wait_for(std::chrono::seconds(5)) == std::future_status::ready) {
        spdlog::info("GetState completed");
        return future.get();
    }
    return {{}, false, "Timeout waiting for state response"};
}

rps::ipc::SetStateResponse GuiSessionManager::setState(const std::string& pluginPath,
                                                       const std::vector<uint8_t>& stateData) {
    std::future<rps::ipc::SetStateResponse> future;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_sessions.find(pluginPath);
        if (it == m_sessions.end()) {
            return {false, "No active GUI session for: " + pluginPath};
        }
        auto* session = it->second.get();

        // Install promise and get future (under session lock)
        {
            std::lock_guard<std::mutex> slock(session->stateMutex);
            session->pendingSetState.emplace();
            future = session->pendingSetState->get_future();
        }

        // Send SetStateRequest
        rps::ipc::Message req;
        req.type = rps::ipc::MessageType::SetStateRequest;
        req.payload = rps::ipc::SetStateRequest{stateData};
        session->connection->sendMessage(req);
    }

    // Wait for response (unlocked — relay loop will fulfill the promise)
    if (future.wait_for(std::chrono::seconds(5)) == std::future_status::ready) {
        spdlog::info("SetState completed");
        return future.get();
    }
    return {false, "Timeout waiting for state response"};
}

rps::ipc::LoadPresetResponse GuiSessionManager::loadPreset(const std::string& pluginPath,
                                                           const std::string& presetId) {
    std::future<rps::ipc::LoadPresetResponse> future;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_sessions.find(pluginPath);
        if (it == m_sessions.end()) {
            return {false, "No active GUI session for: " + pluginPath};
        }
        auto* session = it->second.get();

        // Install promise and get future
        {
            std::lock_guard<std::mutex> slock(session->stateMutex);
            session->pendingLoadPreset.emplace();
            future = session->pendingLoadPreset->get_future();
        }

        // Send LoadPresetRequest
        rps::ipc::Message req;
        req.type = rps::ipc::MessageType::LoadPresetRequest;
        req.payload = rps::ipc::LoadPresetRequest{presetId};
        session->connection->sendMessage(req);
    }

    // Wait for response (unlocked — relay loop will fulfill the promise)
    if (future.wait_for(std::chrono::seconds(5)) == std::future_status::ready) {
        spdlog::info("LoadPreset completed");
        return future.get();
    }
    return {false, "Timeout waiting for preset load response"};
}

rps::audio::SharedAudioRing* GuiSessionManager::getAudioRing(const std::string& pluginPath) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_sessions.find(pluginPath);
    if (it == m_sessions.end() || !it->second->audioRing) return nullptr;
    return it->second->audioRing.get();
}

} // namespace rps::server
