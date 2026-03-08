#include <rps/coordinator/ProcessSlice.hpp>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4100)
#pragma warning(disable: 4244)
#pragma warning(disable: 4245)
#endif
#include <boost/process/v1/child.hpp>
#include <boost/process/v1/args.hpp>
#include <boost/process/v1/io.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <boost/filesystem.hpp>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <spdlog/spdlog.h>

#include <chrono>
#include <fstream>
#include <thread>

namespace bp = boost::process::v1;
namespace bfs = boost::filesystem;

namespace rps::coordinator {

// ---------------------------------------------------------------------------
// PIMPL
// ---------------------------------------------------------------------------

struct ProcessSlice::Impl {
    std::unique_ptr<bp::child> child;
};

// ---------------------------------------------------------------------------
// Constructors / Destructor
// ---------------------------------------------------------------------------

ProcessSlice::ProcessSlice()
    : m_impl(std::make_unique<Impl>()) {}

ProcessSlice::~ProcessSlice() {
    if (m_state == State::Running) {
        terminate();
    }
    cleanup();
}

// ---------------------------------------------------------------------------
// Move operations
// ---------------------------------------------------------------------------

ProcessSlice::ProcessSlice(ProcessSlice&& other) noexcept
    : m_impl(std::move(other.m_impl))
    , m_sliceId(std::move(other.m_sliceId))
    , m_state(other.m_state)
    , m_exitCode(other.m_exitCode)
    , m_graphFilePath(std::move(other.m_graphFilePath))
    , m_shmNames(std::move(other.m_shmNames)) {
    other.m_state = State::NotStarted;
    if (!other.m_impl) {
        other.m_impl = std::make_unique<Impl>();
    }
}

ProcessSlice& ProcessSlice::operator=(ProcessSlice&& other) noexcept {
    if (this != &other) {
        if (m_state == State::Running) {
            terminate();
        }
        cleanup();
        m_impl = std::move(other.m_impl);
        m_sliceId = std::move(other.m_sliceId);
        m_state = other.m_state;
        m_exitCode = other.m_exitCode;
        m_graphFilePath = std::move(other.m_graphFilePath);
        m_shmNames = std::move(other.m_shmNames);
        other.m_state = State::NotStarted;
        if (!other.m_impl) {
            other.m_impl = std::make_unique<Impl>();
        }
    }
    return *this;
}

// ---------------------------------------------------------------------------
// Cleanup helper
// ---------------------------------------------------------------------------

void ProcessSlice::cleanup() {
    if (!m_graphFilePath.empty()) {
        boost::system::error_code ec;
        bfs::remove(m_graphFilePath, ec);
        if (!ec) {
            spdlog::debug("ProcessSlice '{}': removed temp file {}", m_sliceId, m_graphFilePath);
        }
        m_graphFilePath.clear();
    }
}

// ---------------------------------------------------------------------------
// Launch
// ---------------------------------------------------------------------------

bool ProcessSlice::launch(const std::string& hostBinaryPath,
                          const std::string& subgraphJson,
                          const std::vector<std::string>& shmNames) {
    if (m_state == State::Running) {
        spdlog::warn("ProcessSlice '{}': already running", m_sliceId);
        return false;
    }

    // Verify host binary exists
    if (!bfs::exists(hostBinaryPath)) {
        spdlog::error("ProcessSlice '{}': host binary not found: {}", m_sliceId, hostBinaryPath);
        m_state = State::Stopped;
        return false;
    }

    // Write subgraph JSON to a temp file
    auto tempDir = bfs::temp_directory_path();
    auto uuid = boost::uuids::to_string(boost::uuids::random_generator()());
    m_graphFilePath = (tempDir / ("rps_slice_" + uuid + ".json")).string();

    {
        std::ofstream f(m_graphFilePath);
        if (!f) {
            spdlog::error("ProcessSlice '{}': failed to write temp file: {}",
                          m_sliceId, m_graphFilePath);
            m_state = State::Stopped;
            return false;
        }
        f << subgraphJson;
    }

    m_shmNames = shmNames;

    // Build spawn args
    std::vector<std::string> spawnArgs = {
        "--graph-file", m_graphFilePath
    };
    // Pass first shm name as the main audio I/O ring (if any)
    if (!shmNames.empty()) {
        spawnArgs.push_back("--audio-shm");
        spawnArgs.push_back(shmNames[0]);
    }

    spdlog::info("ProcessSlice '{}': launching {} with graph file {} ({} shm bridges)",
                 m_sliceId, hostBinaryPath, m_graphFilePath, shmNames.size());

    // Spawn the child process
    try {
        m_impl->child = std::make_unique<bp::child>(
            hostBinaryPath,
            bp::args(spawnArgs),
            bp::std_in.close(),
            bp::std_out > bp::null,
            bp::std_err > bp::null
        );
    } catch (const std::exception& e) {
        spdlog::error("ProcessSlice '{}': failed to spawn: {}", m_sliceId, e.what());
        cleanup();
        m_state = State::Stopped;
        return false;
    }

    if (m_impl->child && m_impl->child->running()) {
        m_state = State::Running;
        spdlog::info("ProcessSlice '{}': process started (pid={})",
                     m_sliceId, m_impl->child->id());
        return true;
    }

    spdlog::error("ProcessSlice '{}': process exited immediately", m_sliceId);
    m_exitCode = m_impl->child ? m_impl->child->exit_code() : -1;
    m_state = State::Crashed;
    cleanup();
    return false;
}

// ---------------------------------------------------------------------------
// Is running
// ---------------------------------------------------------------------------

bool ProcessSlice::isRunning() const {
    if (m_state != State::Running || !m_impl || !m_impl->child) return false;
    return m_impl->child->running();
}

// ---------------------------------------------------------------------------
// Terminate
// ---------------------------------------------------------------------------

void ProcessSlice::terminate() {
    if (m_state != State::Running || !m_impl || !m_impl->child) {
        return;
    }

    spdlog::info("ProcessSlice '{}': terminating (pid={})",
                 m_sliceId, m_impl->child->id());

    // Send termination signal
    try {
        m_impl->child->terminate();
    } catch (const std::exception& e) {
        spdlog::warn("ProcessSlice '{}': terminate() failed: {}", m_sliceId, e.what());
    }

    // Wait up to 2 seconds for exit
    if (!waitForExit(2000)) {
        spdlog::warn("ProcessSlice '{}': force-killing after timeout", m_sliceId);
        try {
            m_impl->child->terminate();
        } catch (...) {}
    }

    // Capture final state
    if (m_impl->child) {
        std::error_code ec;
        m_impl->child->wait(ec);
        if (!ec) {
            m_exitCode = m_impl->child->exit_code();
        }
    }

    m_state = State::Stopped;
    m_impl->child.reset();
    cleanup();

    spdlog::info("ProcessSlice '{}': terminated (exit={})", m_sliceId, m_exitCode);
}

// ---------------------------------------------------------------------------
// Wait for exit
// ---------------------------------------------------------------------------

bool ProcessSlice::waitForExit(uint32_t timeoutMs) {
    if (!m_impl || !m_impl->child || m_state != State::Running) {
        return true;
    }

    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds(timeoutMs);

    while (std::chrono::steady_clock::now() < deadline) {
        if (!m_impl->child->running()) {
            m_exitCode = m_impl->child->exit_code();
            m_state = (m_exitCode == 0) ? State::Stopped : State::Crashed;
            if (m_state == State::Crashed) {
                spdlog::warn("ProcessSlice '{}': process crashed (exit={})",
                             m_sliceId, m_exitCode);
            }
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    return false;  // Timed out
}

} // namespace rps::coordinator
