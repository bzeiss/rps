#pragma once

#include <rps/ipc/Connection.hpp>
#include <rps/ipc/Messages.hpp>
#include <rps/audio/SharedAudioRing.hpp>
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
    class PluginEvent;
}
namespace grpc {
    template <class W> class ServerWriter;
}

namespace rps::server {

/// Audio configuration for openGui (empty if audio not requested).
struct AudioConfig {
    bool enabled = false;
    uint32_t sampleRate = 48000;
    uint32_t blockSize = 128;
    uint32_t numChannels = 2;
};

/// Manages active plugin GUI sessions. Each session is a child process
/// running a format-specific rps-pluginhost-* binary.
class GuiSessionManager {
public:
    explicit GuiSessionManager(const std::string& hostBinDir);

    /// Open a plugin's GUI in an isolated process.
    /// Blocks until the GUI is closed, streaming events to the gRPC writer.
    void openGui(const std::string& pluginPath, const std::string& format,
                 grpc::ServerWriter<rps::v1::PluginEvent>* writer,
                 const AudioConfig& audioConfig = {});

    /// Close a specific GUI session by plugin path.
    bool closeGui(const std::string& pluginPath);

    /// Close all open GUI sessions (for server shutdown).
    void closeAll();

    /// Get the complete state of a running plugin as an opaque binary blob.
    rps::ipc::GetStateResponse getState(const std::string& pluginPath);

    /// Set (restore) plugin state from a previously saved blob.
    rps::ipc::SetStateResponse setState(const std::string& pluginPath,
                                        const std::vector<uint8_t>& stateData);

    /// Load a preset on a running plugin by its preset id.
    rps::ipc::LoadPresetResponse loadPreset(const std::string& pluginPath,
                                            const std::string& presetId);

    /// Get the audio ring for a session (for gRPC streaming proxy).
    /// Returns nullptr if no audio ring exists for this session.
    rps::audio::SharedAudioRing* getAudioRing(const std::string& pluginPath);

private:
    struct Session {
        std::string pluginPath;
        std::string ipcId;
        std::string shmName;  // Audio shared memory name (empty if no audio)
        std::unique_ptr<rps::audio::SharedAudioRing> audioRing;
        std::unique_ptr<rps::ipc::MessageQueueConnection> connection;
        std::unique_ptr<boost::process::v1::child> process;

        // Response channels — set by callers, fulfilled by openGui relay loop
        std::mutex stateMutex;
        std::optional<std::promise<rps::ipc::GetStateResponse>> pendingGetState;
        std::optional<std::promise<rps::ipc::SetStateResponse>> pendingSetState;
        std::optional<std::promise<rps::ipc::LoadPresetResponse>> pendingLoadPreset;
    };

    std::string m_hostBinDir;
    std::mutex m_mutex;
    std::map<std::string, std::unique_ptr<Session>> m_sessions;

    boost::filesystem::path resolveHostBinary(const std::string& format) const;
};

} // namespace rps::server
