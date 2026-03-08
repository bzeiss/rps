#include <rps/coordinator/ProcessSlice.hpp>

#include <spdlog/spdlog.h>

namespace rps::coordinator {

ProcessSlice::~ProcessSlice() {
    if (m_state == State::Running) {
        terminate();
    }
}

ProcessSlice::ProcessSlice(ProcessSlice&& other) noexcept
    : m_sliceId(std::move(other.m_sliceId))
    , m_state(other.m_state)
    , m_exitCode(other.m_exitCode) {
    other.m_state = State::NotStarted;
}

ProcessSlice& ProcessSlice::operator=(ProcessSlice&& other) noexcept {
    if (this != &other) {
        if (m_state == State::Running) {
            terminate();
        }
        m_sliceId = std::move(other.m_sliceId);
        m_state = other.m_state;
        m_exitCode = other.m_exitCode;
        other.m_state = State::NotStarted;
    }
    return *this;
}

bool ProcessSlice::launch(const std::string& /*hostBinaryPath*/,
                          const std::string& /*subgraphJson*/,
                          const std::vector<std::string>& /*shmNames*/) {
    // Phase 7B: actual process spawning with boost::process
    spdlog::warn("ProcessSlice::launch() is a stub in Phase 7A — "
                 "process spawning will be implemented in Phase 7B");
    m_state = State::Stopped;
    return false;
}

bool ProcessSlice::isRunning() const {
    return m_state == State::Running;
}

void ProcessSlice::terminate() {
    if (m_state == State::Running) {
        // Phase 7B: send shutdown command, then force-kill
        m_state = State::Stopped;
    }
}

bool ProcessSlice::waitForExit(uint32_t /*timeoutMs*/) {
    // Phase 7B: wait for child process
    return m_state != State::Running;
}

} // namespace rps::coordinator
