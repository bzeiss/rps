#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rps::coordinator {

// ---------------------------------------------------------------------------
// ProcessSlice — manages one rps-pluginhost child process
// ---------------------------------------------------------------------------

/// Represents a child process hosting a subgraph slice.
/// Phase 7B: full implementation with boost::process, graceful shutdown.
class ProcessSlice {
public:
    /// Slice state
    enum class State : uint8_t {
        NotStarted,  ///< Not yet launched
        Running,     ///< Process is alive and processing audio
        Stopped,     ///< Gracefully stopped
        Crashed,     ///< Process exited unexpectedly
    };

    ProcessSlice();
    ~ProcessSlice();

    // Non-copyable, movable
    ProcessSlice(const ProcessSlice&) = delete;
    ProcessSlice& operator=(const ProcessSlice&) = delete;
    ProcessSlice(ProcessSlice&& other) noexcept;
    ProcessSlice& operator=(ProcessSlice&& other) noexcept;

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
    void cleanup();

    struct Impl;  // PIMPL — hides boost::process::v1::child
    std::unique_ptr<Impl> m_impl;

    std::string m_sliceId;
    State m_state = State::NotStarted;
    int m_exitCode = 0;
    std::string m_graphFilePath;
    std::vector<std::string> m_shmNames;
};

} // namespace rps::coordinator
