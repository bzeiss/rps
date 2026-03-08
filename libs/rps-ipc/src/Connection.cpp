#include <rps/ipc/Connection.hpp>
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4100)
#pragma warning(disable: 4244)
#pragma warning(disable: 4245)
#endif
#include <boost/interprocess/ipc/message_queue.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#ifdef _MSC_VER
#pragma warning(pop)
#endif
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
            message_queue::remove(txName.c_str());
            message_queue::remove(rxName.c_str());

            m_txQueue = std::make_unique<message_queue>(create_only, txName.c_str(), MAX_NUM_MSGS, MAX_MSG_SIZE);
            m_rxQueue = std::make_unique<message_queue>(create_only, rxName.c_str(), MAX_NUM_MSGS, MAX_MSG_SIZE);
        } else {
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

bool MessageQueueConnection::sendProto(const google::protobuf::MessageLite& msg) {
    try {
        std::string serialized;
        if (!msg.SerializeToString(&serialized)) {
            std::cerr << "IPC Proto Send Error: SerializeToString failed\n";
            return false;
        }

        if (serialized.size() > MAX_MSG_SIZE) {
            std::cerr << "IPC Proto Error: Message too large (" << serialized.size()
                      << " bytes, max " << MAX_MSG_SIZE << ")\n";
            return false;
        }

        m_txQueue->send(serialized.data(), serialized.size(), 0);
        return true;
    } catch (const std::exception& e) {
        std::cerr << "IPC Proto Send Error: " << e.what() << "\n";
        return false;
    }
}

bool MessageQueueConnection::receiveProto(google::protobuf::MessageLite& msg, unsigned int timeoutMs) {
    try {
        std::vector<char> buffer(MAX_MSG_SIZE);
        size_t receivedSize = 0;
        unsigned int priority = 0;

        bool received = false;

        if (timeoutMs == 0) {
            m_rxQueue->receive(buffer.data(), buffer.size(), receivedSize, priority);
            received = true;
        } else {
            boost::posix_time::ptime timeout = boost::posix_time::microsec_clock::universal_time() +
                                               boost::posix_time::milliseconds(timeoutMs);
            received = m_rxQueue->timed_receive(buffer.data(), buffer.size(), receivedSize, priority, timeout);
        }

        if (received && receivedSize > 0) {
            if (!msg.ParseFromArray(buffer.data(), static_cast<int>(receivedSize))) {
                std::cerr << "IPC Proto Receive Error: ParseFromArray failed ("
                          << receivedSize << " bytes)\n";
                return false;
            }
            return true;
        }

        return false;
    } catch (const std::exception& e) {
        std::cerr << "IPC Proto Receive Error (transport): " << e.what() << "\n";
        return false;
    }
}

} // namespace rps::ipc
