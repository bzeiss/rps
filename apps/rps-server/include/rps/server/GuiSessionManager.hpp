#pragma once

#include <rps/ipc/Connection.hpp>
#include <string>
#include <map>
#include <memory>
#include <mutex>

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
    /// @param pluginPath Path to the plugin binary.
    /// @param format Plugin format (e.g. "clap").
    /// @param writer gRPC stream writer for PluginGuiEvent messages.
    void openGui(const std::string& pluginPath, const std::string& format,
                 grpc::ServerWriter<rps::v1::PluginGuiEvent>* writer);

    /// Close a specific GUI session by plugin path.
    /// @return true if a session was found and closed.
    bool closeGui(const std::string& pluginPath);

    /// Close all open GUI sessions (for server shutdown).
    void closeAll();

private:
    struct Session {
        std::string pluginPath;
        std::string ipcId;
        std::unique_ptr<rps::ipc::MessageQueueConnection> connection;
        std::unique_ptr<boost::process::v1::child> process;
    };

    std::string m_hostBinDir;
    std::mutex m_mutex;
    std::map<std::string, std::unique_ptr<Session>> m_sessions;

    boost::filesystem::path resolveHostBinary(const std::string& format) const;
};

} // namespace rps::server
