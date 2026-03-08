#pragma once

#include <rps/ipc/Messages.hpp>
#include <boost/interprocess/ipc/message_queue.hpp>
#include <boost/json.hpp>
#include <google/protobuf/message_lite.h>
#include <string>
#include <optional>
#include <memory>
#include <vector>

namespace rps::ipc {

class Connection {
public:
    virtual ~Connection() = default;

    // Send a message (JSON-based, for scanner IPC)
    virtual bool sendMessage(const Message& msg) = 0;

    // Receive a message (JSON-based, for scanner IPC, blocking with timeout)
    virtual std::optional<Message> receiveMessage(unsigned int timeoutMs = 0) = 0;

    // Send a protobuf message (binary, for pluginhost IPC)
    virtual bool sendProto(const google::protobuf::MessageLite& msg) = 0;

    // Receive a protobuf message (binary, for pluginhost IPC, blocking with timeout)
    virtual bool receiveProto(google::protobuf::MessageLite& msg, unsigned int timeoutMs = 0) = 0;
};

// We use Boost.Interprocess message_queue for fast local IPC.
// It requires a unique name per connection.
class MessageQueueConnection : public Connection {
public:
    // Create a new queue (typically done by orchestrator)
    static std::unique_ptr<MessageQueueConnection> createServer(const std::string& name);

    // Connect to an existing queue (typically done by scanner)
    static std::unique_ptr<MessageQueueConnection> createClient(const std::string& name);

    ~MessageQueueConnection() override;

    bool sendMessage(const Message& msg) override;
    std::optional<Message> receiveMessage(unsigned int timeoutMs = 0) override;

    bool sendProto(const google::protobuf::MessageLite& msg) override;
    bool receiveProto(google::protobuf::MessageLite& msg, unsigned int timeoutMs = 0) override;

private:
    MessageQueueConnection(const std::string& name, bool isServer);
    
    std::string m_name;
    bool m_isServer;
    
    // We actually need TWO queues for bidirectional communication:
    // 1. Orchestrator -> Scanner (Requests)
    // 2. Scanner -> Orchestrator (Results/Progress)
    std::unique_ptr<boost::interprocess::message_queue> m_txQueue;
    std::unique_ptr<boost::interprocess::message_queue> m_rxQueue;
};

} // namespace rps::ipc

