#include <rps/server/GuiSessionManager.hpp>

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
    // Validate format
    if (format != "clap" && format != "vst3") {
        // Future: "vst2", "au", etc.
        return {};
    }

    // Try unified binary first, then format-specific fallback
    std::vector<std::string> candidates = {
        "rps-pluginhost",            // unified binary (preferred)
        "rps-pluginhost-" + format,  // format-specific fallback
    };

#ifdef _WIN32
    for (auto& c : candidates) c += ".exe";
#endif

    for (const auto& binaryName : candidates) {
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
        const bool hasShm = !shmName.empty();
        const bool hasDev = !audioConfig.audioDevice.empty();

        if (hasShm && hasDev) {
            child = std::make_unique<bp::child>(
                hostBin.string(),
                "--ipc-id", ipcId,
                "--plugin-path", pluginPath,
                "--format", format,
                "--audio-shm", shmName,
                "--audio-device", audioConfig.audioDevice
            );
        } else if (hasShm) {
            child = std::make_unique<bp::child>(
                hostBin.string(),
                "--ipc-id", ipcId,
                "--plugin-path", pluginPath,
                "--format", format,
                "--audio-shm", shmName
            );
        } else if (hasDev) {
            child = std::make_unique<bp::child>(
                hostBin.string(),
                "--ipc-id", ipcId,
                "--plugin-path", pluginPath,
                "--format", format,
                "--audio-device", audioConfig.audioDevice
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

        rps::host::HostEvent evt;
        if (!currentSession->connection->receiveProto(evt, 500)) {
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

        // Handle the HostEvent oneof
        if (evt.has_plugin_loaded()) {
            const auto& loaded = evt.plugin_loaded();
            spdlog::info("Plugin loaded (headless): '{}'", loaded.plugin_name());

            // Forward as PluginLoaded event to gRPC
            rps::v1::PluginEvent event;
            auto* opened = event.mutable_gui_opened();
            opened->set_plugin_name(loaded.plugin_name());
            opened->set_width(0);  // No GUI yet
            opened->set_height(0);
            writer->Write(event);

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
        } else if (evt.has_gui_opened()) {
            rps::v1::PluginEvent event;
            *event.mutable_gui_opened() = evt.gui_opened();
            writer->Write(event);
            spdlog::info("Plugin GUI opened: {} ({}x{})",
                         evt.gui_opened().plugin_name(),
                         evt.gui_opened().width(),
                         evt.gui_opened().height());
        } else if (evt.has_gui_closed()) {
            rps::v1::PluginEvent event;
            *event.mutable_gui_closed() = evt.gui_closed();
            writer->Write(event);
            spdlog::info("Plugin GUI closed: {} (reason: {})",
                         pluginPath, evt.gui_closed().reason());
            // session_ended means the host process is shutting down
            if (evt.gui_closed().reason() == "session_ended" ||
                evt.gui_closed().reason() == "crash") {
                done = true;
            }
        } else if (evt.has_parameter_list()) {
            rps::v1::PluginEvent event;
            *event.mutable_parameter_list() = evt.parameter_list();
            writer->Write(event);
            spdlog::info("Sent ParameterList ({} params)",
                         evt.parameter_list().parameters_size());
        } else if (evt.has_parameter_updates()) {
            rps::v1::PluginEvent event;
            *event.mutable_parameter_updates() = evt.parameter_updates();
            writer->Write(event);
        } else if (evt.has_get_state_result()) {
            std::lock_guard<std::mutex> slock(currentSession->stateMutex);
            if (currentSession->pendingGetState) {
                currentSession->pendingGetState->set_value(
                    std::move(*evt.mutable_get_state_result()));
                currentSession->pendingGetState.reset();
                spdlog::info("Fulfilled pendingGetState");
            }
        } else if (evt.has_set_state_result()) {
            std::lock_guard<std::mutex> slock(currentSession->stateMutex);
            if (currentSession->pendingSetState) {
                currentSession->pendingSetState->set_value(
                    std::move(*evt.mutable_set_state_result()));
                currentSession->pendingSetState.reset();
                spdlog::info("Fulfilled pendingSetState");
            }
        } else if (evt.has_preset_list()) {
            rps::v1::PluginEvent event;
            *event.mutable_preset_list() = evt.preset_list();
            writer->Write(event);
            spdlog::info("Sent PresetList ({} presets)",
                         evt.preset_list().presets_size());
        } else if (evt.has_load_preset_result()) {
            std::lock_guard<std::mutex> slock(currentSession->stateMutex);
            if (currentSession->pendingLoadPreset) {
                currentSession->pendingLoadPreset->set_value(
                    std::move(*evt.mutable_load_preset_result()));
                currentSession->pendingLoadPreset.reset();
                spdlog::info("Fulfilled pendingLoadPreset");
            }
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

    // Send CloseGuiCommand via IPC — closes the window but keeps the process alive
    rps::host::HostCommand cmd;
    cmd.mutable_close_gui();
    it->second->connection->sendProto(cmd);

    return true;
}

bool GuiSessionManager::showGui(const std::string& pluginPath) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_sessions.find(pluginPath);
    if (it == m_sessions.end()) {
        return false;
    }

    // Send ShowGuiCommand via IPC — opens the GUI window
    rps::host::HostCommand cmd;
    cmd.mutable_show_gui();
    it->second->connection->sendProto(cmd);

    return true;
}

bool GuiSessionManager::closeSession(const std::string& pluginPath) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_sessions.find(pluginPath);
    if (it == m_sessions.end()) {
        return false;
    }

    // Send CloseSessionCommand via IPC — terminates the host process
    rps::host::HostCommand cmd;
    cmd.mutable_close_session();
    it->second->connection->sendProto(cmd);

    return true;
}

void GuiSessionManager::closeAll() {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& [path, session] : m_sessions) {
        // Send CloseSessionCommand (terminates the host process)
        rps::host::HostCommand cmd;
        cmd.mutable_close_session();
        try {
            session->connection->sendProto(cmd);
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

rps::host::GetStateResult GuiSessionManager::getState(const std::string& pluginPath) {
    std::future<rps::host::GetStateResult> future;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_sessions.find(pluginPath);
        if (it == m_sessions.end()) {
            rps::host::GetStateResult errResp;
            errResp.set_success(false);
            errResp.set_error("No active GUI session for: " + pluginPath);
            return errResp;
        }
        auto* session = it->second.get();

        // Install promise and get future (under session lock)
        {
            std::lock_guard<std::mutex> slock(session->stateMutex);
            session->pendingGetState.emplace();
            future = session->pendingGetState->get_future();
        }

        // Send GetStateCommand
        rps::host::HostCommand cmd;
        cmd.mutable_get_state();
        session->connection->sendProto(cmd);
    }

    // Wait for response (unlocked — relay loop will fulfill the promise)
    if (future.wait_for(std::chrono::seconds(5)) == std::future_status::ready) {
        spdlog::info("GetState completed");
        return future.get();
    }
    rps::host::GetStateResult timeoutResp;
    timeoutResp.set_success(false);
    timeoutResp.set_error("Timeout waiting for state response");
    return timeoutResp;
}

rps::host::SetStateResult GuiSessionManager::setState(const std::string& pluginPath,
                                                       const std::string& stateData) {
    std::future<rps::host::SetStateResult> future;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_sessions.find(pluginPath);
        if (it == m_sessions.end()) {
            rps::host::SetStateResult errResp;
            errResp.set_success(false);
            errResp.set_error("No active GUI session for: " + pluginPath);
            return errResp;
        }
        auto* session = it->second.get();

        // Install promise and get future (under session lock)
        {
            std::lock_guard<std::mutex> slock(session->stateMutex);
            session->pendingSetState.emplace();
            future = session->pendingSetState->get_future();
        }

        // Send SetStateCommand
        rps::host::HostCommand cmd;
        cmd.mutable_set_state()->set_state_data(stateData);
        session->connection->sendProto(cmd);
    }

    // Wait for response (unlocked — relay loop will fulfill the promise)
    if (future.wait_for(std::chrono::seconds(5)) == std::future_status::ready) {
        spdlog::info("SetState completed");
        return future.get();
    }
    rps::host::SetStateResult timeoutResp;
    timeoutResp.set_success(false);
    timeoutResp.set_error("Timeout waiting for state response");
    return timeoutResp;
}

rps::host::LoadPresetResult GuiSessionManager::loadPreset(const std::string& pluginPath,
                                                           const std::string& presetId) {
    std::future<rps::host::LoadPresetResult> future;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_sessions.find(pluginPath);
        if (it == m_sessions.end()) {
            rps::host::LoadPresetResult errResp;
            errResp.set_success(false);
            errResp.set_error("No active GUI session for: " + pluginPath);
            return errResp;
        }
        auto* session = it->second.get();

        // Install promise and get future
        {
            std::lock_guard<std::mutex> slock(session->stateMutex);
            session->pendingLoadPreset.emplace();
            future = session->pendingLoadPreset->get_future();
        }

        // Send LoadPresetCommand
        rps::host::HostCommand cmd;
        cmd.mutable_load_preset()->set_preset_id(presetId);
        session->connection->sendProto(cmd);
    }

    // Wait for response (unlocked — relay loop will fulfill the promise)
    if (future.wait_for(std::chrono::seconds(5)) == std::future_status::ready) {
        spdlog::info("LoadPreset completed");
        return future.get();
    }
    rps::host::LoadPresetResult timeoutResp;
    timeoutResp.set_success(false);
    timeoutResp.set_error("Timeout waiting for preset load response");
    return timeoutResp;
}

rps::audio::SharedAudioRing* GuiSessionManager::getAudioRing(const std::string& pluginPath) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_sessions.find(pluginPath);
    if (it == m_sessions.end() || !it->second->audioRing) return nullptr;
    return it->second->audioRing.get();
}

} // namespace rps::server
