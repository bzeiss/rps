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

namespace bp = boost::process::v1;

namespace rps::server {

GuiSessionManager::GuiSessionManager(const std::string& hostBinDir)
    : m_hostBinDir(hostBinDir) {}

boost::filesystem::path GuiSessionManager::resolveHostBinary(const std::string& format) const {
    std::string binaryName;
    if (format == "clap") {
        binaryName = "rps-pluginhost-clap";
    } else {
        // Future: "vst3" -> "rps-pluginhost-vst3", etc.
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
                                 grpc::ServerWriter<rps::v1::PluginGuiEvent>* writer) {
    // Check if already open
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_sessions.count(pluginPath)) {
            rps::v1::PluginGuiEvent event;
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
        rps::v1::PluginGuiEvent event;
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
        rps::v1::PluginGuiEvent event;
        auto* err = event.mutable_gui_error();
        err->set_error("ipc_failed");
        err->set_details("Failed to create IPC message queue.");
        writer->Write(event);
        return;
    }

    spdlog::info("Launching plugin host: {} for {} (ipc: {})", hostBin.string(), pluginPath, ipcId);

    // Spawn the host process
    std::unique_ptr<bp::child> child;
    try {
        child = std::make_unique<bp::child>(
            hostBin.string(),
            "--ipc-id", ipcId,
            "--plugin-path", pluginPath,
            "--format", format
        );
    } catch (const std::exception& e) {
        rps::v1::PluginGuiEvent event;
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
    session->connection = std::move(connection);
    session->process = std::move(child);

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_sessions[pluginPath] = std::move(session);
    }

    // Wait for events from the worker via IPC and forward to gRPC
    bool done = false;
    while (!done) {
        rps::ipc::MessageQueueConnection* conn = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_sessions.find(pluginPath);
            if (it == m_sessions.end()) break;
            conn = it->second->connection.get();
        }
        if (!conn) break;

        auto msg = conn->receiveMessage(500);
        if (!msg) {
            // Check if process is still running
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_sessions.find(pluginPath);
            if (it == m_sessions.end()) break;
            if (!it->second->process->running()) {
                // Process exited unexpectedly
                rps::v1::PluginGuiEvent event;
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
                rps::v1::PluginGuiEvent event;
                auto* opened = event.mutable_gui_opened();
                opened->set_plugin_name(evt.pluginName);
                opened->set_width(evt.width);
                opened->set_height(evt.height);
                writer->Write(event);
                spdlog::info("Plugin GUI opened: {} ({}x{})", evt.pluginName, evt.width, evt.height);
                break;
            }
            case rps::ipc::MessageType::GuiClosedEvent: {
                auto& evt = std::get<rps::ipc::GuiClosedEvent>(msg->payload);
                rps::v1::PluginGuiEvent event;
                auto* closed = event.mutable_gui_closed();
                closed->set_reason(evt.reason);
                writer->Write(event);
                spdlog::info("Plugin GUI closed: {} (reason: {})", pluginPath, evt.reason);
                done = true;
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

} // namespace rps::server
