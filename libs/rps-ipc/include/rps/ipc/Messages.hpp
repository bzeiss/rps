#pragma once

#include <string>
#include <vector>
#include <map>
#include <variant>
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4100)
#pragma warning(disable: 4244)
#pragma warning(disable: 4245)
#endif
#include <boost/json.hpp>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace rps::ipc {

enum class MessageType {
    ScanRequest,
    ScanResult,
    ProgressEvent,
    ErrorMessage,
    // GUI lifecycle
    OpenGuiRequest,
    GuiOpenedEvent,
    GuiClosedEvent,
    CloseGuiRequest,
    // Headless-first lifecycle
    PluginLoadedEvent,
    ShowGuiRequest,
    CloseSessionRequest,
    // Parameter streaming
    ParameterListEvent,
    ParameterValuesEvent,
    // State save/restore
    GetStateRequest,
    GetStateResponse,
    SetStateRequest,
    SetStateResponse,
    // Preset enumeration/loading
    PresetListEvent,
    LoadPresetRequest,
    LoadPresetResponse,
    PresetLoadedEvent
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
    std::string format;      // "vst2", "vst3", "clap", "aax", "au", "lv2"
    std::string scanMethod;  // "moduleinfo.json" or "factory" — how metadata was obtained
    uint32_t numInputs = 0;
    uint32_t numOutputs = 0;
    std::vector<ParameterInfo> parameters;
    std::map<std::string, std::string> extraData; // Format-specific metadata (e.g. AAX IDs)
};

struct ProgressEvent {
    std::string status;
    int progressPercentage = 0; // 0-100
};

struct ErrorMessage {
    std::string error;
    std::string details;
};

// GUI lifecycle messages (server <-> plugin host worker)
struct OpenGuiRequest {
    std::string pluginPath;
    std::string format;
};

struct GuiOpenedEvent {
    std::string pluginName;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct GuiClosedEvent {
    std::string reason; // "user", "host", "crash"
};

struct CloseGuiRequest {};

// Headless-first lifecycle messages

/// Sent by host after loadPlugin() completes (before any GUI).
struct PluginLoadedEvent {
    std::string pluginName;
    std::string pluginVendor;
    bool hasGui = false;
};

/// Request from server to host to show the GUI window.
struct ShowGuiRequest {};

/// Request from server to host to terminate the session entirely.
struct CloseSessionRequest {};

// ---------------------------------------------------------------------------
// Parameter streaming messages (plugin host worker -> server)
// ---------------------------------------------------------------------------

/// Flags for PluginParameterInfo
enum PluginParameterFlags : uint32_t {
    kParamFlagNone     = 0,
    kParamFlagStepped  = 1 << 0,  // Integer/discrete values only
    kParamFlagHidden   = 1 << 1,  // Should not be shown to user
    kParamFlagReadOnly = 1 << 2,  // Cannot be changed by host
    kParamFlagBypass   = 1 << 3,  // Bypass parameter
    kParamFlagEnum     = 1 << 4,  // Enumerated values (also implies stepped)
};

/// Universal parameter descriptor, format-agnostic.
/// All values are in plain scale (not normalized).
struct PluginParameterInfo {
    std::string id;           // Unique identifier (string for cross-format compat)
    uint32_t index = 0;       // Positional index
    std::string name;         // Human-readable display name
    std::string module;       // Hierarchical group path, empty if N/A
    double minValue = 0.0;
    double maxValue = 1.0;
    double defaultValue = 0.0;
    double currentValue = 0.0;
    std::string displayText;  // Formatted display (e.g. "2.3 kHz")
    uint32_t flags = 0;       // PluginParameterFlags bitmask
};

/// Sent once after the plugin GUI opens, containing the full parameter list.
struct ParameterListEvent {
    std::vector<PluginParameterInfo> parameters;
};

/// Single parameter value change.
struct ParameterValueUpdate {
    std::string paramId;
    double value = 0.0;
    std::string displayText;
};

/// Sent periodically: delta updates for changed parameter values.
struct ParameterValuesEvent {
    std::vector<ParameterValueUpdate> updates;
};

// ---------------------------------------------------------------------------
// State save/restore messages (server <-> plugin host worker)
// ---------------------------------------------------------------------------

/// Request from server to host to save state.
struct GetStateRequest {};

/// Response from host with serialized plugin state.
struct GetStateResponse {
    std::vector<uint8_t> stateData;  // Opaque binary blob from the plugin
    bool success = false;
    std::string error;
};

/// Request from server to host to restore state.
struct SetStateRequest {
    std::vector<uint8_t> stateData;
};

/// Response from host confirming state restore.
struct SetStateResponse {
    bool success = false;
    std::string error;
};

// ---------------------------------------------------------------------------
// Preset enumeration/loading messages (server <-> plugin host worker)
// ---------------------------------------------------------------------------

/// Flags for PresetInfo
enum PresetFlags : uint32_t {
    kPresetFlagNone    = 0,
    kPresetFlagFactory = 1 << 0,  // Factory/built-in preset
    kPresetFlagUser    = 1 << 1,  // User-created preset
    kPresetFlagFavorite= 1 << 2,  // Marked as favorite
};

/// Universal preset descriptor, format-agnostic.
struct PresetInfo {
    std::string id;        // Unique key: load_key (CLAP), program index (VST3), etc.
    std::string name;      // Human-readable display name
    std::string category;  // Category/module path
    std::string creator;   // Author (optional)
    std::string location;  // File path or container (for CLAP location-based loading)
    uint32_t locationKind = 0; // CLAP location kind (file=0, plugin=1)
    uint32_t index = 0;    // Ordering index
    uint32_t flags = 0;    // PresetFlags bitmask
};

/// Sent once after the plugin GUI opens, containing available presets.
struct PresetListEvent {
    std::vector<PresetInfo> presets;
};

/// Request from server to host to load a preset.
struct LoadPresetRequest {
    std::string presetId;  // The id from PresetInfo
};

/// Response from host confirming preset load.
struct LoadPresetResponse {
    bool success = false;
    std::string error;
};

/// Notification that a preset was loaded (e.g. user picked one in plugin UI).
struct PresetLoadedEvent {
    std::string presetId;
    std::string presetName;
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

void tag_invoke(boost::json::value_from_tag, boost::json::value& jv, const OpenGuiRequest& req);
OpenGuiRequest tag_invoke(boost::json::value_to_tag<OpenGuiRequest>, const boost::json::value& jv);

void tag_invoke(boost::json::value_from_tag, boost::json::value& jv, const GuiOpenedEvent& evt);
GuiOpenedEvent tag_invoke(boost::json::value_to_tag<GuiOpenedEvent>, const boost::json::value& jv);

void tag_invoke(boost::json::value_from_tag, boost::json::value& jv, const GuiClosedEvent& evt);
GuiClosedEvent tag_invoke(boost::json::value_to_tag<GuiClosedEvent>, const boost::json::value& jv);

void tag_invoke(boost::json::value_from_tag, boost::json::value& jv, const CloseGuiRequest& req);
CloseGuiRequest tag_invoke(boost::json::value_to_tag<CloseGuiRequest>, const boost::json::value& jv);

void tag_invoke(boost::json::value_from_tag, boost::json::value& jv, const PluginLoadedEvent& evt);
PluginLoadedEvent tag_invoke(boost::json::value_to_tag<PluginLoadedEvent>, const boost::json::value& jv);

void tag_invoke(boost::json::value_from_tag, boost::json::value& jv, const ShowGuiRequest& req);
ShowGuiRequest tag_invoke(boost::json::value_to_tag<ShowGuiRequest>, const boost::json::value& jv);

void tag_invoke(boost::json::value_from_tag, boost::json::value& jv, const CloseSessionRequest& req);
CloseSessionRequest tag_invoke(boost::json::value_to_tag<CloseSessionRequest>, const boost::json::value& jv);

void tag_invoke(boost::json::value_from_tag, boost::json::value& jv, const PluginParameterInfo& p);
PluginParameterInfo tag_invoke(boost::json::value_to_tag<PluginParameterInfo>, const boost::json::value& jv);

void tag_invoke(boost::json::value_from_tag, boost::json::value& jv, const ParameterListEvent& evt);
ParameterListEvent tag_invoke(boost::json::value_to_tag<ParameterListEvent>, const boost::json::value& jv);

void tag_invoke(boost::json::value_from_tag, boost::json::value& jv, const ParameterValueUpdate& u);
ParameterValueUpdate tag_invoke(boost::json::value_to_tag<ParameterValueUpdate>, const boost::json::value& jv);

void tag_invoke(boost::json::value_from_tag, boost::json::value& jv, const ParameterValuesEvent& evt);
ParameterValuesEvent tag_invoke(boost::json::value_to_tag<ParameterValuesEvent>, const boost::json::value& jv);

void tag_invoke(boost::json::value_from_tag, boost::json::value& jv, const GetStateRequest& req);
GetStateRequest tag_invoke(boost::json::value_to_tag<GetStateRequest>, const boost::json::value& jv);

void tag_invoke(boost::json::value_from_tag, boost::json::value& jv, const GetStateResponse& resp);
GetStateResponse tag_invoke(boost::json::value_to_tag<GetStateResponse>, const boost::json::value& jv);

void tag_invoke(boost::json::value_from_tag, boost::json::value& jv, const SetStateRequest& req);
SetStateRequest tag_invoke(boost::json::value_to_tag<SetStateRequest>, const boost::json::value& jv);

void tag_invoke(boost::json::value_from_tag, boost::json::value& jv, const SetStateResponse& resp);
SetStateResponse tag_invoke(boost::json::value_to_tag<SetStateResponse>, const boost::json::value& jv);

void tag_invoke(boost::json::value_from_tag, boost::json::value& jv, const PresetInfo& p);
PresetInfo tag_invoke(boost::json::value_to_tag<PresetInfo>, const boost::json::value& jv);

void tag_invoke(boost::json::value_from_tag, boost::json::value& jv, const PresetListEvent& evt);
PresetListEvent tag_invoke(boost::json::value_to_tag<PresetListEvent>, const boost::json::value& jv);

void tag_invoke(boost::json::value_from_tag, boost::json::value& jv, const LoadPresetRequest& req);
LoadPresetRequest tag_invoke(boost::json::value_to_tag<LoadPresetRequest>, const boost::json::value& jv);

void tag_invoke(boost::json::value_from_tag, boost::json::value& jv, const LoadPresetResponse& resp);
LoadPresetResponse tag_invoke(boost::json::value_to_tag<LoadPresetResponse>, const boost::json::value& jv);

void tag_invoke(boost::json::value_from_tag, boost::json::value& jv, const PresetLoadedEvent& evt);
PresetLoadedEvent tag_invoke(boost::json::value_to_tag<PresetLoadedEvent>, const boost::json::value& jv);

// Wrapper for any message
struct Message {
    MessageType type;
    std::variant<ScanRequest, ScanResult, ProgressEvent, ErrorMessage,
                 OpenGuiRequest, GuiOpenedEvent, GuiClosedEvent, CloseGuiRequest,
                 PluginLoadedEvent, ShowGuiRequest, CloseSessionRequest,
                 ParameterListEvent, ParameterValuesEvent,
                 GetStateRequest, GetStateResponse, SetStateRequest, SetStateResponse,
                 PresetListEvent, LoadPresetRequest, LoadPresetResponse, PresetLoadedEvent> payload;
};

void tag_invoke(boost::json::value_from_tag, boost::json::value& jv, const Message& msg);
Message tag_invoke(boost::json::value_to_tag<Message>, const boost::json::value& jv);

} // namespace rps::ipc
