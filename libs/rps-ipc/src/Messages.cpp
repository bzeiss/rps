#include <rps/ipc/Messages.hpp>
#include <stdexcept>

namespace rps::ipc {

void tag_invoke(boost::json::value_from_tag, boost::json::value& jv, const ScanRequest& req) {
    jv = {
        {"pluginPath", req.pluginPath},
        {"format", req.format},
        {"requiresUI", req.requiresUI}
    };
}

ScanRequest tag_invoke(boost::json::value_to_tag<ScanRequest>, const boost::json::value& jv) {
    auto const& obj = jv.as_object();
    return {
        boost::json::value_to<std::string>(obj.at("pluginPath")),
        boost::json::value_to<std::string>(obj.at("format")),
        boost::json::value_to<bool>(obj.at("requiresUI"))
    };
}

void tag_invoke(boost::json::value_from_tag, boost::json::value& jv, const ParameterInfo& p) {
    jv = {
        {"id", p.id},
        {"name", p.name},
        {"defaultValue", p.defaultValue}
    };
}

ParameterInfo tag_invoke(boost::json::value_to_tag<ParameterInfo>, const boost::json::value& jv) {
    auto const& obj = jv.as_object();
    return {
        boost::json::value_to<uint32_t>(obj.at("id")),
        boost::json::value_to<std::string>(obj.at("name")),
        boost::json::value_to<double>(obj.at("defaultValue"))
    };
}

void tag_invoke(boost::json::value_from_tag, boost::json::value& jv, const ScanResult& res) {
    jv = {
        {"name", res.name},
        {"vendor", res.vendor},
        {"version", res.version},
        {"uid", res.uid},
        {"description", res.description},
        {"url", res.url},
        {"category", res.category},
        {"numInputs", res.numInputs},
        {"numOutputs", res.numOutputs},
        {"parameters", boost::json::value_from(res.parameters)}
    };
}

ScanResult tag_invoke(boost::json::value_to_tag<ScanResult>, const boost::json::value& jv) {
    const auto& obj = jv.as_object();
    ScanResult res;
    res.name = obj.at("name").as_string().c_str();
    res.vendor = obj.at("vendor").as_string().c_str();
    res.version = obj.at("version").as_string().c_str();
    
    // Optional new fields (for backward compatibility if missing in old JSON)
    if (obj.contains("uid")) res.uid = obj.at("uid").as_string().c_str();
    if (obj.contains("description")) res.description = obj.at("description").as_string().c_str();
    if (obj.contains("url")) res.url = obj.at("url").as_string().c_str();
    if (obj.contains("category")) res.category = obj.at("category").as_string().c_str();

    res.numInputs = obj.at("numInputs").to_number<uint32_t>();
    res.numOutputs = obj.at("numOutputs").to_number<uint32_t>();
    res.parameters = boost::json::value_to<std::vector<ParameterInfo>>(obj.at("parameters"));
    return res;
}

void tag_invoke(boost::json::value_from_tag, boost::json::value& jv, const ProgressEvent& evt) {
    jv = {
        {"status", evt.status},
        {"progressPercentage", evt.progressPercentage}
    };
}

ProgressEvent tag_invoke(boost::json::value_to_tag<ProgressEvent>, const boost::json::value& jv) {
    auto const& obj = jv.as_object();
    return {
        boost::json::value_to<std::string>(obj.at("status")),
        boost::json::value_to<int>(obj.at("progressPercentage"))
    };
}

void tag_invoke(boost::json::value_from_tag, boost::json::value& jv, const ErrorMessage& err) {
    jv = {
        {"error", err.error},
        {"details", err.details}
    };
}

ErrorMessage tag_invoke(boost::json::value_to_tag<ErrorMessage>, const boost::json::value& jv) {
    auto const& obj = jv.as_object();
    return {
        boost::json::value_to<std::string>(obj.at("error")),
        boost::json::value_to<std::string>(obj.at("details"))
    };
}

void tag_invoke(boost::json::value_from_tag, boost::json::value& jv, const Message& msg) {
    boost::json::object obj;
    obj["type"] = static_cast<int>(msg.type);
    
    std::visit([&obj](auto&& arg) {
        obj["payload"] = boost::json::value_from(arg);
    }, msg.payload);
    
    jv = std::move(obj);
}

Message tag_invoke(boost::json::value_to_tag<Message>, const boost::json::value& jv) {
    auto const& obj = jv.as_object();
    auto type = static_cast<MessageType>(obj.at("type").as_int64());
    auto const& payload_val = obj.at("payload");
    
    Message msg;
    msg.type = type;
    
    switch (type) {
        case MessageType::ScanRequest:
            msg.payload = boost::json::value_to<ScanRequest>(payload_val);
            break;
        case MessageType::ScanResult:
            msg.payload = boost::json::value_to<ScanResult>(payload_val);
            break;
        case MessageType::ProgressEvent:
            msg.payload = boost::json::value_to<ProgressEvent>(payload_val);
            break;
        case MessageType::ErrorMessage:
            msg.payload = boost::json::value_to<ErrorMessage>(payload_val);
            break;
        default:
            throw std::runtime_error("Unknown MessageType");
    }
    
    return msg;
}

} // namespace rps::ipc
