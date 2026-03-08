#pragma once

#include <rps/ipc/Messages.hpp>
#include <boost/interprocess/ipc/message_queue.hpp>
#include <google/protobuf/message_lite.h>
#include <string>
#include <optional>
#include <memory>
#include <vector>

namespace rps::ipc {

class Connection {
public:
    virtual ~Connection() = default;

    // Send a protobuf message (binary, length-prefixed)
    virtual bool sendProto(const google::protobuf::MessageLite& msg) = 0;

    // Receive a protobuf message (binary, length-prefixed, blocking with timeout)
    virtual bool receiveProto(google::protobuf::MessageLite& msg, unsigned int timeoutMs = 0) = 0;
};

// Boost.Interprocess message_queue — used by the scanner path.
class MessageQueueConnection : public Connection {
public:
    static std::unique_ptr<MessageQueueConnection> createServer(const std::string& name);
    static std::unique_ptr<MessageQueueConnection> createClient(const std::string& name);

    ~MessageQueueConnection() override;

    bool sendProto(const google::protobuf::MessageLite& msg) override;
    bool receiveProto(google::protobuf::MessageLite& msg, unsigned int timeoutMs = 0) override;

private:
    MessageQueueConnection(const std::string& name, bool isServer);
    
    std::string m_name;
    bool m_isServer;
    
    // TWO queues for bidirectional communication:
    // 1. Orchestrator -> Scanner (Commands)
    // 2. Scanner -> Orchestrator (Events)
    std::unique_ptr<boost::interprocess::message_queue> m_txQueue;
    std::unique_ptr<boost::interprocess::message_queue> m_rxQueue;
};

} // namespace rps::ipc
