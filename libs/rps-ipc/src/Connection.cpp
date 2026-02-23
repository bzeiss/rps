#include <rps/ipc/Connection.hpp>
#include <boost/interprocess/ipc/message_queue.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <iostream>
#include <stdexcept>

namespace rps::ipc {

using namespace boost::interprocess;

constexpr size_t MAX_MSG_SIZE = 1048576; // 1 MB — must accommodate plugins with many params
constexpr size_t MAX_NUM_MSGS = 8;       // Low: protocol is request/response + progress events

MessageQueueConnection::MessageQueueConnection(const std::string& name, bool isServer)
    : m_name(name), m_isServer(isServer) {
    
    std::string txName = isServer ? name + "_req" : name + "_res";
    std::string rxName = isServer ? name + "_res" : name + "_req";

    try {
        if (isServer) {
            // Server creates the queues
            message_queue::remove(txName.c_str());
            message_queue::remove(rxName.c_str());

            m_txQueue = std::make_unique<message_queue>(create_only, txName.c_str(), MAX_NUM_MSGS, MAX_MSG_SIZE);
            m_rxQueue = std::make_unique<message_queue>(create_only, rxName.c_str(), MAX_NUM_MSGS, MAX_MSG_SIZE);
        } else {
            // Client connects to the existing queues
            m_txQueue = std::make_unique<message_queue>(open_only, txName.c_str());
            m_rxQueue = std::make_unique<message_queue>(open_only, rxName.c_str());
        }
    } catch (interprocess_exception& ex) {
        throw std::runtime_error("Failed to initialize IPC MessageQueueConnection: " + std::string(ex.what()));
    }
}

MessageQueueConnection::~MessageQueueConnection() {
    if (m_isServer) {
        std::string txName = m_name + "_req";
        std::string rxName = m_name + "_res";
        message_queue::remove(txName.c_str());
        message_queue::remove(rxName.c_str());
    }
}

std::unique_ptr<MessageQueueConnection> MessageQueueConnection::createServer(const std::string& name) {
    return std::unique_ptr<MessageQueueConnection>(new MessageQueueConnection(name, true));
}

std::unique_ptr<MessageQueueConnection> MessageQueueConnection::createClient(const std::string& name) {
    return std::unique_ptr<MessageQueueConnection>(new MessageQueueConnection(name, false));
}

bool MessageQueueConnection::sendMessage(const Message& msg) {
    try {
        boost::json::value jv = boost::json::value_from(msg);
        std::string serialized = boost::json::serialize(jv);

        if (serialized.size() > MAX_MSG_SIZE) {
            std::cerr << "IPC Error: Message too large to send (" << serialized.size()
                      << " bytes, max " << MAX_MSG_SIZE << ")\n";
            return false;
        }

        // Log payload size for diagnostics (only for large messages)
        if (serialized.size() > 65536) {
            std::cerr << "[ipc] Sending large message: " << serialized.size() << " bytes\n";
        }

        m_txQueue->send(serialized.data(), serialized.size(), 0);
        return true;
    } catch (const std::exception& e) {
        std::cerr << "IPC Send Error: " << e.what() << "\n";
        return false;
    }
}

std::optional<Message> MessageQueueConnection::receiveMessage(unsigned int timeoutMs) {
    try {
        std::vector<char> buffer(MAX_MSG_SIZE);
        size_t receivedSize = 0;
        unsigned int priority = 0;

        bool received = false;

        if (timeoutMs == 0) {
            // Blocking wait indefinitely
            m_rxQueue->receive(buffer.data(), buffer.size(), receivedSize, priority);
            received = true;
        } else {
            // Timeout wait
            boost::posix_time::ptime timeout = boost::posix_time::microsec_clock::universal_time() +
                                               boost::posix_time::milliseconds(timeoutMs);
            received = m_rxQueue->timed_receive(buffer.data(), buffer.size(), receivedSize, priority, timeout);
        }

        if (received && receivedSize > 0) {
            std::string serialized(buffer.data(), receivedSize);
            boost::json::value jv = boost::json::parse(serialized);
            return boost::json::value_to<Message>(jv);
        }

        return std::nullopt;
    } catch (const std::exception& e) {
        std::cerr << "IPC Receive Error: " << e.what() << "\n";
        return std::nullopt;
    }
}

} // namespace rps::ipc
