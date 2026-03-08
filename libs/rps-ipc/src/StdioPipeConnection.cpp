#include <rps/ipc/StdioPipeConnection.hpp>
#include <google/protobuf/message_lite.h>
#include <iostream>
#include <cstring>

#ifdef _WIN32
#include <io.h>      // _get_osfhandle
#else
#include <unistd.h>
#include <poll.h>
#include <errno.h>
#endif

namespace rps::ipc {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

#ifdef _WIN32
StdioPipeConnection::StdioPipeConnection(HANDLE writeHandle, HANDLE readHandle)
    : m_writeHandle(writeHandle), m_readHandle(readHandle) {}
#else
StdioPipeConnection::StdioPipeConnection(int writeFd, int readFd)
    : m_writeFd(writeFd), m_readFd(readFd) {}
#endif

std::unique_ptr<StdioPipeConnection> StdioPipeConnection::fromStdio(int savedStdoutFd, int stdinFd) {
#ifdef _WIN32
    HANDLE writeHandle = reinterpret_cast<HANDLE>(_get_osfhandle(savedStdoutFd));
    HANDLE readHandle = reinterpret_cast<HANDLE>(_get_osfhandle(stdinFd));
    return std::make_unique<StdioPipeConnection>(writeHandle, readHandle);
#else
    return std::make_unique<StdioPipeConnection>(savedStdoutFd, stdinFd);
#endif
}

StdioPipeConnection::~StdioPipeConnection() = default;

// ---------------------------------------------------------------------------
// Low-level I/O (native handles only — no iostream)
// ---------------------------------------------------------------------------

bool StdioPipeConnection::writeAll(const void* data, size_t n) {
    const auto* p = static_cast<const char*>(data);
    size_t remaining = n;
    while (remaining > 0) {
#ifdef _WIN32
        DWORD written = 0;
        if (!WriteFile(m_writeHandle, p, static_cast<DWORD>(remaining), &written, nullptr)) {
            std::cerr << "[pipe] WriteFile failed: " << GetLastError() << "\n";
            return false;
        }
        if (written == 0) return false;
#else
        auto written = ::write(m_writeFd, p, remaining);
        if (written <= 0) {
            if (errno == EINTR) continue;
            std::cerr << "[pipe] write() failed: " << strerror(errno) << "\n";
            return false;
        }
#endif
        p += written;
        remaining -= static_cast<size_t>(written);
    }
    return true;
}

bool StdioPipeConnection::readAll(void* data, size_t n) {
    auto* p = static_cast<char*>(data);
    size_t remaining = n;
    while (remaining > 0) {
#ifdef _WIN32
        DWORD bytesRead = 0;
        if (!ReadFile(m_readHandle, p, static_cast<DWORD>(remaining), &bytesRead, nullptr) || bytesRead == 0) {
            return false;  // EOF or error
        }
#else
        auto bytesRead = ::read(m_readFd, p, remaining);
        if (bytesRead <= 0) {
            if (bytesRead < 0 && errno == EINTR) continue;
            return false;  // EOF or error
        }
#endif
        p += bytesRead;
        remaining -= static_cast<size_t>(bytesRead);
    }
    return true;
}

bool StdioPipeConnection::waitForData(unsigned int timeoutMs) {
    if (timeoutMs == 0) return true;  // No timeout = will block on readAll

#ifdef _WIN32
    auto startTime = GetTickCount64();
    while (true) {
        DWORD available = 0;
        if (!PeekNamedPipe(m_readHandle, nullptr, 0, nullptr, &available, nullptr)) {
            return false;  // Pipe broken
        }
        if (available > 0) return true;

        auto elapsed = GetTickCount64() - startTime;
        if (elapsed >= timeoutMs) return false;
        Sleep(std::min(static_cast<DWORD>(10), static_cast<DWORD>(timeoutMs - elapsed)));
    }
#else
    struct pollfd pfd;
    pfd.fd = m_readFd;
    pfd.events = POLLIN;
    int ret = ::poll(&pfd, 1, static_cast<int>(timeoutMs));
    return ret > 0 && (pfd.revents & POLLIN);
#endif
}

// ---------------------------------------------------------------------------
// Protobuf send/receive (length-prefixed framing)
// ---------------------------------------------------------------------------

bool StdioPipeConnection::sendProto(const google::protobuf::MessageLite& msg) {
    try {
        std::string serialized;
        if (!msg.SerializeToString(&serialized)) {
            std::cerr << "[pipe] sendProto: SerializeToString failed\n";
            return false;
        }

        // Write length prefix (4 bytes, little-endian)
        uint32_t len = static_cast<uint32_t>(serialized.size());
        if (!writeAll(&len, sizeof(len))) {
            std::cerr << "[pipe] sendProto: failed to write length prefix\n";
            return false;
        }

        // Write payload
        if (!writeAll(serialized.data(), serialized.size())) {
            std::cerr << "[pipe] sendProto: failed to write payload (" << len << " bytes)\n";
            return false;
        }

        return true;
    } catch (const std::exception& e) {
        std::cerr << "[pipe] sendProto exception: " << e.what() << "\n";
        return false;
    }
}

bool StdioPipeConnection::receiveProto(google::protobuf::MessageLite& msg, unsigned int timeoutMs) {
    try {
        // Check for data availability with timeout
        if (timeoutMs > 0 && !waitForData(timeoutMs)) {
            return false;  // Timeout — no data available
        }

        // Read length prefix (4 bytes, little-endian)
        uint32_t len = 0;
        if (!readAll(&len, sizeof(len))) {
            return false;  // EOF or error
        }

        // Sanity check
        constexpr uint32_t MAX_PROTO_SIZE = 16 * 1024 * 1024; // 16 MB
        if (len == 0 || len > MAX_PROTO_SIZE) {
            std::cerr << "[pipe] receiveProto: invalid message length: " << len << "\n";
            return false;
        }

        // Read payload
        std::string buffer(len, '\0');
        if (!readAll(buffer.data(), len)) {
            std::cerr << "[pipe] receiveProto: failed to read payload (" << len << " bytes)\n";
            return false;
        }

        // Parse protobuf
        if (!msg.ParseFromString(buffer)) {
            std::cerr << "[pipe] receiveProto: ParseFromString failed (" << len << " bytes)\n";
            return false;
        }

        return true;
    } catch (const std::exception& e) {
        std::cerr << "[pipe] receiveProto exception: " << e.what() << "\n";
        return false;
    }
}

// ---------------------------------------------------------------------------
// JSON methods — not supported on pipe transport
// ---------------------------------------------------------------------------

bool StdioPipeConnection::sendMessage(const Message& /*msg*/) {
    std::cerr << "[pipe] sendMessage: JSON not supported on pipe transport\n";
    return false;
}

std::optional<Message> StdioPipeConnection::receiveMessage(unsigned int /*timeoutMs*/) {
    std::cerr << "[pipe] receiveMessage: JSON not supported on pipe transport\n";
    return std::nullopt;
}

} // namespace rps::ipc
