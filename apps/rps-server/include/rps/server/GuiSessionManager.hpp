#pragma once

#include <rps/ipc/Connection.hpp>
#include <rps/ipc/Messages.hpp>
#include <string>
#include <map>
#include <memory>
#include <mutex>
#include <future>
#include <optional>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4100)
#pragma warning(disable: 4244)
#endif
#include <boost/process/v1/child.hpp>
#include <boost/filesystem.hpp>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

// Forward declare gRPC types
namespace rps::v1 {
    class PluginGuiEvent;
}
namespace grpc {
    template <class W> class ServerWriter;
}

namespace rps::server {

/// Manages active plugin GUI sessions. Each session is a child process
/// running a format-specific rps-pluginhost-* binary.
class GuiSessionManager {
public:
    explicit GuiSessionManager(const std::string& hostBinDir);

    /// Open a plugin's GUI in an isolated process.
    /// Blocks until the GUI is closed, streaming events to the gRPC writer.
    void openGui(const std::string& pluginPath, const std::string& format,
                 grpc::ServerWriter<rps::v1::PluginGuiEvent>* writer);

    /// Close a specific GUI session by plugin path.
    bool closeGui(const std::string& pluginPath);

    /// Close all open GUI sessions (for server shutdown).
    void closeAll();

    /// Get the complete state of a running plugin as an opaque binary blob.
    rps::ipc::GetStateResponse getState(const std::string& pluginPath);

    /// Set (restore) plugin state from a previously saved blob.
    rps::ipc::SetStateResponse setState(const std::string& pluginPath,
                                        const std::vector<uint8_t>& stateData);

private:
    struct Session {
        std::string pluginPath;
        std::string ipcId;
        std::unique_ptr<rps::ipc::MessageQueueConnection> connection;
        std::unique_ptr<boost::process::v1::child> process;

        // State response channels — set by getState/setState, fulfilled by openGui relay loop
        std::mutex stateMutex;
        std::optional<std::promise<rps::ipc::GetStateResponse>> pendingGetState;
        std::optional<std::promise<rps::ipc::SetStateResponse>> pendingSetState;
    };

    std::string m_hostBinDir;
    std::mutex m_mutex;
    std::map<std::string, std::unique_ptr<Session>> m_sessions;

    boost::filesystem::path resolveHostBinary(const std::string& format) const;
};

} // namespace rps::server
