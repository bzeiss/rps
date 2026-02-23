#pragma once

#include <string>
#include <vector>
#include <variant>
#include <boost/json.hpp>

namespace rps::ipc {

enum class MessageType {
    ScanRequest,
    ScanResult,
    ProgressEvent,
    ErrorMessage
};

struct ScanRequest {
    std::string pluginPath;
    std::string format; // "vst3", "clap", etc.
    bool requiresUI = false;
};

struct ParameterInfo {
    uint32_t id;
    std::string name;
    double defaultValue;
};

struct ScanResult {
    std::string name;
    std::string vendor;
    std::string version;
    std::string uid;
    std::string description;
    std::string url;
    std::string category;
    std::string scanMethod;  // "moduleinfo.json" or "factory" — how metadata was obtained
    uint32_t numInputs = 0;
    uint32_t numOutputs = 0;
    std::vector<ParameterInfo> parameters;
};

struct ProgressEvent {
    std::string status;
    int progressPercentage = 0; // 0-100
};

struct ErrorMessage {
    std::string error;
    std::string details;
};

// JSON Serialization Declarations
void tag_invoke(boost::json::value_from_tag, boost::json::value& jv, const ScanRequest& req);
ScanRequest tag_invoke(boost::json::value_to_tag<ScanRequest>, const boost::json::value& jv);

void tag_invoke(boost::json::value_from_tag, boost::json::value& jv, const ParameterInfo& p);
ParameterInfo tag_invoke(boost::json::value_to_tag<ParameterInfo>, const boost::json::value& jv);

void tag_invoke(boost::json::value_from_tag, boost::json::value& jv, const ScanResult& res);
ScanResult tag_invoke(boost::json::value_to_tag<ScanResult>, const boost::json::value& jv);

void tag_invoke(boost::json::value_from_tag, boost::json::value& jv, const ProgressEvent& evt);
ProgressEvent tag_invoke(boost::json::value_to_tag<ProgressEvent>, const boost::json::value& jv);

void tag_invoke(boost::json::value_from_tag, boost::json::value& jv, const ErrorMessage& err);
ErrorMessage tag_invoke(boost::json::value_to_tag<ErrorMessage>, const boost::json::value& jv);

// Wrapper for any message
struct Message {
    MessageType type;
    std::variant<ScanRequest, ScanResult, ProgressEvent, ErrorMessage> payload;
};

void tag_invoke(boost::json::value_from_tag, boost::json::value& jv, const Message& msg);
Message tag_invoke(boost::json::value_to_tag<Message>, const boost::json::value& jv);

} // namespace rps::ipc
