#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rps::audio {
class SharedAudioRing;
}

namespace rps::coordinator {

// ---------------------------------------------------------------------------
// ProcessSlice — manages one rps-pluginhost child process
// ---------------------------------------------------------------------------

/// Represents a child process hosting a subgraph slice.
/// Phase 7A: interface + stub. Phase 7B: full implementation with
/// boost::process, health monitoring, and restart-on-crash.
class ProcessSlice {
public:
    /// Slice state
    enum class State : uint8_t {
        NotStarted,  ///< Not yet launched
        Running,     ///< Process is alive and processing audio
        Stopped,     ///< Gracefully stopped
        Crashed,     ///< Process exited unexpectedly
    };

    ProcessSlice() = default;
    ~ProcessSlice();

    // Non-copyable, movable
    ProcessSlice(const ProcessSlice&) = delete;
    ProcessSlice& operator=(const ProcessSlice&) = delete;
    ProcessSlice(ProcessSlice&&) noexcept;
    ProcessSlice& operator=(ProcessSlice&&) noexcept;

    /// Launch the child process with the given subgraph JSON.
    /// @param hostBinaryPath  Path to rps-pluginhost binary.
    /// @param subgraphJson    Serialized Graph JSON to pass via --graph-file.
    /// @param shmNames        Shared memory segment names for Send/Receive bridges.
    /// @return true on success.
    bool launch(const std::string& hostBinaryPath,
                const std::string& subgraphJson,
                const std::vector<std::string>& shmNames);

    /// Check if the child process is currently running.
    bool isRunning() const;

    /// Get the current state.
    State state() const { return m_state; }

    /// Get the exit code (valid only after process has stopped).
    int exitCode() const { return m_exitCode; }

    /// Gracefully terminate the child process.
    void terminate();

    /// Wait for the child process to exit (with timeout in ms).
    /// Returns true if the process exited within the timeout.
    bool waitForExit(uint32_t timeoutMs = 5000);

    /// Get the slice ID (assigned by coordinator).
    const std::string& sliceId() const { return m_sliceId; }
    void setSliceId(const std::string& id) { m_sliceId = id; }

private:
    std::string m_sliceId;
    State m_state = State::NotStarted;
    int m_exitCode = 0;

    // Phase 7B will add:
    // std::unique_ptr<boost::process::v1::child> m_child;
    // IPC pipe handles
    // SharedAudioRing references
};

} // namespace rps::coordinator
