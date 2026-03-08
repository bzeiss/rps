#pragma once

#include <rps/ipc/Connection.hpp>
#include <google/protobuf/message_lite.h>
#include <string>
#include <optional>
#include <memory>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
using HANDLE = void*;  // Not used on POSIX, but simplifies struct layout
#endif

namespace rps::ipc {

/// Length-prefixed protobuf connection over pipes (stdin/stdout or boost::process pipes).
///
/// Wire format per message:
///   [4 bytes: uint32_t payload length, little-endian]
///   [N bytes: serialized protobuf payload]
///
/// Always uses native OS handles for I/O (not iostream), ensuring reliable
/// timeout support via PeekNamedPipe (Windows) / poll (POSIX).
class StdioPipeConnection : public Connection {
public:
    /// Create from native OS handles (works for both server and client side).
#ifdef _WIN32
    StdioPipeConnection(HANDLE writeHandle, HANDLE readHandle);
#else
    StdioPipeConnection(int writeFd, int readFd);
#endif

    /// Create a child-side connection from the process's stdin/stdout.
    /// @param savedStdoutFd The original stdout fd saved before redirect.
    /// @param stdinFd The stdin fd (default: 0 = inherited stdin).
    static std::unique_ptr<StdioPipeConnection> fromStdio(int savedStdoutFd, int stdinFd = 0);

    ~StdioPipeConnection() override;

    // Protobuf methods (primary API)
    bool sendProto(const google::protobuf::MessageLite& msg) override;
    bool receiveProto(google::protobuf::MessageLite& msg, unsigned int timeoutMs = 0) override;

private:
    bool writeAll(const void* data, size_t n);
    bool readAll(void* data, size_t n);
    bool waitForData(unsigned int timeoutMs);

#ifdef _WIN32
    HANDLE m_writeHandle = INVALID_HANDLE_VALUE;
    HANDLE m_readHandle = INVALID_HANDLE_VALUE;
#else
    int m_writeFd = -1;
    int m_readFd = -1;
#endif
};

} // namespace rps::ipc
