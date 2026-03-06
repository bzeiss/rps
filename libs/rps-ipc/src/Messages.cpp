#include <rps/ipc/Messages.hpp>
#include <stdexcept>
#include <algorithm>

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
        {"format", res.format},
        {"scanMethod", res.scanMethod},
        {"numInputs", res.numInputs},
        {"numOutputs", res.numOutputs},
        {"parameters", boost::json::value_from(res.parameters)},
        {"extraData", [&]() {
            boost::json::object obj;
            for (const auto& [k, v] : res.extraData)
                obj[k] = v;
            return obj;
        }()}
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
    if (obj.contains("format")) res.format = obj.at("format").as_string().c_str();
    if (obj.contains("scanMethod")) res.scanMethod = obj.at("scanMethod").as_string().c_str();

    res.numInputs = obj.at("numInputs").to_number<uint32_t>();
    res.numOutputs = obj.at("numOutputs").to_number<uint32_t>();
    res.parameters = boost::json::value_to<std::vector<ParameterInfo>>(obj.at("parameters"));
    if (obj.contains("extraData") && obj.at("extraData").is_object()) {
        for (const auto& [k, v] : obj.at("extraData").as_object()) {
            if (v.is_string())
                res.extraData[std::string(k)] = std::string(v.as_string());
        }
    }
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

// --- GUI lifecycle messages ---

void tag_invoke(boost::json::value_from_tag, boost::json::value& jv, const OpenGuiRequest& req) {
    jv = {
        {"pluginPath", req.pluginPath},
        {"format", req.format}
    };
}

OpenGuiRequest tag_invoke(boost::json::value_to_tag<OpenGuiRequest>, const boost::json::value& jv) {
    auto const& obj = jv.as_object();
    return {
        boost::json::value_to<std::string>(obj.at("pluginPath")),
        boost::json::value_to<std::string>(obj.at("format"))
    };
}

void tag_invoke(boost::json::value_from_tag, boost::json::value& jv, const GuiOpenedEvent& evt) {
    jv = {
        {"pluginName", evt.pluginName},
        {"width", evt.width},
        {"height", evt.height}
    };
}

GuiOpenedEvent tag_invoke(boost::json::value_to_tag<GuiOpenedEvent>, const boost::json::value& jv) {
    auto const& obj = jv.as_object();
    return {
        boost::json::value_to<std::string>(obj.at("pluginName")),
        obj.at("width").to_number<uint32_t>(),
        obj.at("height").to_number<uint32_t>()
    };
}

void tag_invoke(boost::json::value_from_tag, boost::json::value& jv, const GuiClosedEvent& evt) {
    jv = {
        {"reason", evt.reason}
    };
}

GuiClosedEvent tag_invoke(boost::json::value_to_tag<GuiClosedEvent>, const boost::json::value& jv) {
    auto const& obj = jv.as_object();
    return {
        boost::json::value_to<std::string>(obj.at("reason"))
    };
}

void tag_invoke(boost::json::value_from_tag, boost::json::value& jv, const CloseGuiRequest&) {
    jv = boost::json::object{};
}

CloseGuiRequest tag_invoke(boost::json::value_to_tag<CloseGuiRequest>, const boost::json::value&) {
    return {};
}

// --- Parameter streaming messages ---

void tag_invoke(boost::json::value_from_tag, boost::json::value& jv, const PluginParameterInfo& p) {
    jv = {
        {"id", p.id},
        {"index", p.index},
        {"name", p.name},
        {"module", p.module},
        {"minValue", p.minValue},
        {"maxValue", p.maxValue},
        {"defaultValue", p.defaultValue},
        {"currentValue", p.currentValue},
        {"displayText", p.displayText},
        {"flags", p.flags}
    };
}

PluginParameterInfo tag_invoke(boost::json::value_to_tag<PluginParameterInfo>, const boost::json::value& jv) {
    auto const& obj = jv.as_object();
    PluginParameterInfo p;
    p.id = boost::json::value_to<std::string>(obj.at("id"));
    p.index = obj.at("index").to_number<uint32_t>();
    p.name = boost::json::value_to<std::string>(obj.at("name"));
    if (obj.contains("module")) p.module = boost::json::value_to<std::string>(obj.at("module"));
    p.minValue = obj.at("minValue").to_number<double>();
    p.maxValue = obj.at("maxValue").to_number<double>();
    p.defaultValue = obj.at("defaultValue").to_number<double>();
    p.currentValue = obj.at("currentValue").to_number<double>();
    if (obj.contains("displayText")) p.displayText = boost::json::value_to<std::string>(obj.at("displayText"));
    p.flags = obj.at("flags").to_number<uint32_t>();
    return p;
}

void tag_invoke(boost::json::value_from_tag, boost::json::value& jv, const ParameterListEvent& evt) {
    jv = {
        {"parameters", boost::json::value_from(evt.parameters)}
    };
}

ParameterListEvent tag_invoke(boost::json::value_to_tag<ParameterListEvent>, const boost::json::value& jv) {
    auto const& obj = jv.as_object();
    return {
        boost::json::value_to<std::vector<PluginParameterInfo>>(obj.at("parameters"))
    };
}

void tag_invoke(boost::json::value_from_tag, boost::json::value& jv, const ParameterValueUpdate& u) {
    jv = {
        {"paramId", u.paramId},
        {"value", u.value},
        {"displayText", u.displayText}
    };
}

ParameterValueUpdate tag_invoke(boost::json::value_to_tag<ParameterValueUpdate>, const boost::json::value& jv) {
    auto const& obj = jv.as_object();
    return {
        boost::json::value_to<std::string>(obj.at("paramId")),
        obj.at("value").to_number<double>(),
        boost::json::value_to<std::string>(obj.at("displayText"))
    };
}

void tag_invoke(boost::json::value_from_tag, boost::json::value& jv, const ParameterValuesEvent& evt) {
    jv = {
        {"updates", boost::json::value_from(evt.updates)}
    };
}

ParameterValuesEvent tag_invoke(boost::json::value_to_tag<ParameterValuesEvent>, const boost::json::value& jv) {
    auto const& obj = jv.as_object();
    return {
        boost::json::value_to<std::vector<ParameterValueUpdate>>(obj.at("updates"))
    };
}

// --- Base64 helpers for binary state blobs ---
namespace {
static constexpr char kBase64Chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64Encode(const std::vector<uint8_t>& data) {
    std::string result;
    result.reserve(4 * ((data.size() + 2) / 3));
    size_t i = 0;
    for (; i + 2 < data.size(); i += 3) {
        result += kBase64Chars[(data[i] >> 2) & 0x3F];
        result += kBase64Chars[((data[i] & 0x3) << 4) | ((data[i+1] >> 4) & 0xF)];
        result += kBase64Chars[((data[i+1] & 0xF) << 2) | ((data[i+2] >> 6) & 0x3)];
        result += kBase64Chars[data[i+2] & 0x3F];
    }
    if (i < data.size()) {
        result += kBase64Chars[(data[i] >> 2) & 0x3F];
        if (i + 1 < data.size()) {
            result += kBase64Chars[((data[i] & 0x3) << 4) | ((data[i+1] >> 4) & 0xF)];
            result += kBase64Chars[((data[i+1] & 0xF) << 2)];
        } else {
            result += kBase64Chars[((data[i] & 0x3) << 4)];
            result += '=';
        }
        result += '=';
    }
    return result;
}

std::vector<uint8_t> base64Decode(const std::string& encoded) {
    static constexpr uint8_t kDecodeTable[256] = {
        64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
        64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
        64,64,64,64,64,64,64,64,64,64,64,62,64,64,64,63,
        52,53,54,55,56,57,58,59,60,61,64,64,64, 0,64,64,
        64, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,64,64,64,64,64,
        64,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,64,64,64,64,64,
        64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
        64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
        64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
        64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
        64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
        64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
        64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
        64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64
    };
    std::vector<uint8_t> result;
    result.reserve(3 * encoded.size() / 4);
    uint32_t buf = 0;
    int bits = 0;
    for (char c : encoded) {
        if (c == '=' || c == '\n' || c == '\r') continue;
        uint8_t d = kDecodeTable[static_cast<uint8_t>(c)];
        if (d == 64) continue;
        buf = (buf << 6) | d;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            result.push_back(static_cast<uint8_t>((buf >> bits) & 0xFF));
        }
    }
    return result;
}
} // anonymous namespace

void tag_invoke(boost::json::value_from_tag, boost::json::value& jv, const GetStateRequest&) {
    jv = boost::json::object{};
}

GetStateRequest tag_invoke(boost::json::value_to_tag<GetStateRequest>, const boost::json::value&) {
    return {};
}

void tag_invoke(boost::json::value_from_tag, boost::json::value& jv, const GetStateResponse& resp) {
    jv = {
        {"stateData", base64Encode(resp.stateData)},
        {"success", resp.success},
        {"error", resp.error}
    };
}

GetStateResponse tag_invoke(boost::json::value_to_tag<GetStateResponse>, const boost::json::value& jv) {
    auto const& obj = jv.as_object();
    GetStateResponse resp;
    resp.stateData = base64Decode(boost::json::value_to<std::string>(obj.at("stateData")));
    resp.success = obj.at("success").as_bool();
    if (obj.contains("error")) resp.error = boost::json::value_to<std::string>(obj.at("error"));
    return resp;
}

void tag_invoke(boost::json::value_from_tag, boost::json::value& jv, const SetStateRequest& req) {
    jv = {
        {"stateData", base64Encode(req.stateData)}
    };
}

SetStateRequest tag_invoke(boost::json::value_to_tag<SetStateRequest>, const boost::json::value& jv) {
    auto const& obj = jv.as_object();
    return {
        base64Decode(boost::json::value_to<std::string>(obj.at("stateData")))
    };
}

void tag_invoke(boost::json::value_from_tag, boost::json::value& jv, const SetStateResponse& resp) {
    jv = {
        {"success", resp.success},
        {"error", resp.error}
    };
}

SetStateResponse tag_invoke(boost::json::value_to_tag<SetStateResponse>, const boost::json::value& jv) {
    auto const& obj = jv.as_object();
    SetStateResponse resp;
    resp.success = obj.at("success").as_bool();
    if (obj.contains("error")) resp.error = boost::json::value_to<std::string>(obj.at("error"));
    return resp;
}

// ---------------------------------------------------------------------------
// Preset enumeration/loading serialization
// ---------------------------------------------------------------------------

void tag_invoke(boost::json::value_from_tag, boost::json::value& jv, const PresetInfo& p) {
    jv = {
        {"id", p.id},
        {"name", p.name},
        {"category", p.category},
        {"creator", p.creator},
        {"location", p.location},
        {"locationKind", p.locationKind},
        {"index", p.index},
        {"flags", p.flags}
    };
}

PresetInfo tag_invoke(boost::json::value_to_tag<PresetInfo>, const boost::json::value& jv) {
    auto const& obj = jv.as_object();
    PresetInfo p;
    p.id = boost::json::value_to<std::string>(obj.at("id"));
    p.name = boost::json::value_to<std::string>(obj.at("name"));
    if (obj.contains("category")) p.category = boost::json::value_to<std::string>(obj.at("category"));
    if (obj.contains("creator")) p.creator = boost::json::value_to<std::string>(obj.at("creator"));
    if (obj.contains("location")) p.location = boost::json::value_to<std::string>(obj.at("location"));
    if (obj.contains("locationKind")) p.locationKind = boost::json::value_to<uint32_t>(obj.at("locationKind"));
    if (obj.contains("index")) p.index = boost::json::value_to<uint32_t>(obj.at("index"));
    if (obj.contains("flags")) p.flags = boost::json::value_to<uint32_t>(obj.at("flags"));
    return p;
}

void tag_invoke(boost::json::value_from_tag, boost::json::value& jv, const PresetListEvent& evt) {
    boost::json::array arr;
    for (const auto& p : evt.presets) {
        arr.push_back(boost::json::value_from(p));
    }
    jv = {{"presets", std::move(arr)}};
}

PresetListEvent tag_invoke(boost::json::value_to_tag<PresetListEvent>, const boost::json::value& jv) {
    auto const& obj = jv.as_object();
    PresetListEvent evt;
    for (const auto& v : obj.at("presets").as_array()) {
        evt.presets.push_back(boost::json::value_to<PresetInfo>(v));
    }
    return evt;
}

void tag_invoke(boost::json::value_from_tag, boost::json::value& jv, const LoadPresetRequest& req) {
    jv = {{"presetId", req.presetId}};
}

LoadPresetRequest tag_invoke(boost::json::value_to_tag<LoadPresetRequest>, const boost::json::value& jv) {
    auto const& obj = jv.as_object();
    return {boost::json::value_to<std::string>(obj.at("presetId"))};
}

void tag_invoke(boost::json::value_from_tag, boost::json::value& jv, const LoadPresetResponse& resp) {
    jv = {
        {"success", resp.success},
        {"error", resp.error}
    };
}

LoadPresetResponse tag_invoke(boost::json::value_to_tag<LoadPresetResponse>, const boost::json::value& jv) {
    auto const& obj = jv.as_object();
    LoadPresetResponse resp;
    resp.success = obj.at("success").as_bool();
    if (obj.contains("error")) resp.error = boost::json::value_to<std::string>(obj.at("error"));
    return resp;
}

void tag_invoke(boost::json::value_from_tag, boost::json::value& jv, const PresetLoadedEvent& evt) {
    jv = {
        {"presetId", evt.presetId},
        {"presetName", evt.presetName}
    };
}

PresetLoadedEvent tag_invoke(boost::json::value_to_tag<PresetLoadedEvent>, const boost::json::value& jv) {
    auto const& obj = jv.as_object();
    PresetLoadedEvent evt;
    evt.presetId = boost::json::value_to<std::string>(obj.at("presetId"));
    if (obj.contains("presetName")) evt.presetName = boost::json::value_to<std::string>(obj.at("presetName"));
    return evt;
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
        case MessageType::OpenGuiRequest:
            msg.payload = boost::json::value_to<OpenGuiRequest>(payload_val);
            break;
        case MessageType::GuiOpenedEvent:
            msg.payload = boost::json::value_to<GuiOpenedEvent>(payload_val);
            break;
        case MessageType::GuiClosedEvent:
            msg.payload = boost::json::value_to<GuiClosedEvent>(payload_val);
            break;
        case MessageType::CloseGuiRequest:
            msg.payload = boost::json::value_to<CloseGuiRequest>(payload_val);
            break;
        case MessageType::ParameterListEvent:
            msg.payload = boost::json::value_to<rps::ipc::ParameterListEvent>(payload_val);
            break;
        case MessageType::ParameterValuesEvent:
            msg.payload = boost::json::value_to<rps::ipc::ParameterValuesEvent>(payload_val);
            break;
        case MessageType::GetStateRequest:
            msg.payload = boost::json::value_to<GetStateRequest>(payload_val);
            break;
        case MessageType::GetStateResponse:
            msg.payload = boost::json::value_to<GetStateResponse>(payload_val);
            break;
        case MessageType::SetStateRequest:
            msg.payload = boost::json::value_to<SetStateRequest>(payload_val);
            break;
        case MessageType::SetStateResponse:
            msg.payload = boost::json::value_to<SetStateResponse>(payload_val);
            break;
        case MessageType::PresetListEvent:
            msg.payload = boost::json::value_to<rps::ipc::PresetListEvent>(payload_val);
            break;
        case MessageType::LoadPresetRequest:
            msg.payload = boost::json::value_to<LoadPresetRequest>(payload_val);
            break;
        case MessageType::LoadPresetResponse:
            msg.payload = boost::json::value_to<LoadPresetResponse>(payload_val);
            break;
        case MessageType::PresetLoadedEvent:
            msg.payload = boost::json::value_to<PresetLoadedEvent>(payload_val);
            break;
        default:
            throw std::runtime_error("Unknown MessageType");
    }
    
    return msg;
}

} // namespace rps::ipc

