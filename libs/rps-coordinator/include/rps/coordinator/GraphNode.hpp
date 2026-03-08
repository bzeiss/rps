#pragma once

#include <rps/coordinator/ChannelFormat.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace rps::coordinator {

// ---------------------------------------------------------------------------
// Node types — every node in the graph is one of these.
// ---------------------------------------------------------------------------

enum class NodeType : uint32_t {
    Input           = 0,   // Main audio input from external source
    Output          = 1,   // Main audio output to external sink
    Plugin          = 2,   // Wraps a loaded plugin instance
    Mixer           = 3,   // Sums multiple inputs (64-bit accumulation)
    ChannelRouter   = 4,   // Routes/reorders specific channels
    Downmix         = 5,   // Speaker-layout-aware fold-down
    Send            = 6,   // Writes audio to shared memory for another process
    Receive         = 7,   // Reads audio from shared memory written by a Send
    SidechainInput  = 8,   // External sidechain audio source
    Gain            = 9,   // Per-channel gain / mute / bypass
};

/// Convert NodeType to string for serialization.
std::string_view nodeTypeToString(NodeType type);

/// Parse string to NodeType. Returns std::nullopt on failure.
std::optional<NodeType> nodeTypeFromString(std::string_view name);

// ---------------------------------------------------------------------------
// Port direction
// ---------------------------------------------------------------------------

enum class PortDirection : uint8_t {
    Input  = 0,
    Output = 1,
};

// ---------------------------------------------------------------------------
// Port descriptor — describes one input or output on a node
// ---------------------------------------------------------------------------

struct PortDescriptor {
    uint32_t       index = 0;
    PortDirection  direction = PortDirection::Input;
    ChannelLayout  layout;
};

// ---------------------------------------------------------------------------
// Node configuration (per node-type settings)
// ---------------------------------------------------------------------------

/// Configuration for an InputNode.
struct InputNodeConfig {
    ChannelLayout layout;
    std::string   shmName;          // Shared memory segment name (for external audio)
};

/// Configuration for an OutputNode.
struct OutputNodeConfig {
    ChannelLayout layout;
    std::string   shmName;
};

/// Configuration for a PluginNode.
struct PluginNodeConfig {
    std::string pluginPath;         // Filesystem path to the plugin binary
    std::string format;             // "vst3", "clap", etc.
};

/// Configuration for a MixerNode.
struct MixerNodeConfig {
    ChannelLayout outputLayout;
    uint32_t      numInputs = 2;
    std::vector<float> inputGains;  // Per-input gain (default 1.0)
};

/// Configuration for a ChannelRouterNode.
struct ChannelRouterNodeConfig {
    ChannelLayout inputLayout;
    ChannelLayout outputLayout;
    /// Mapping: outputChannel[i] = inputChannel[routing[i]]
    /// Length must equal output channel count.
    std::vector<uint32_t> routing;
};

/// Configuration for a DownmixNode.
struct DownmixNodeConfig {
    ChannelLayout inputLayout;
    ChannelLayout outputLayout;    // Must be strictly fewer channels
};

/// Configuration for a SendNode.
struct SendNodeConfig {
    ChannelLayout layout;
    std::string   shmName;         // Named shared memory segment
};

/// Configuration for a ReceiveNode.
struct ReceiveNodeConfig {
    ChannelLayout layout;
    std::string   shmName;         // Must match a SendNode's shmName
};

/// Configuration for a SidechainInputNode.
struct SidechainInputNodeConfig {
    ChannelLayout layout;
    std::string   shmName;
};

/// Configuration for a GainNode.
struct GainNodeConfig {
    ChannelLayout layout;
    float         gain = 1.0f;
    bool          mute = false;
    bool          bypass = false;
};

// ---------------------------------------------------------------------------
// GraphNode — a node in the processing graph
// ---------------------------------------------------------------------------

struct GraphNode {
    std::string   id;
    NodeType      type = NodeType::Input;

    // Port descriptors (populated based on type + config)
    std::vector<PortDescriptor> inputPorts;
    std::vector<PortDescriptor> outputPorts;

    // Optional linked node id (for future linked node pairs)
    std::optional<std::string> linkedNodeId;

    // Type-specific configuration (exactly one should be set, matching type)
    std::optional<InputNodeConfig>          inputConfig;
    std::optional<OutputNodeConfig>         outputConfig;
    std::optional<PluginNodeConfig>         pluginConfig;
    std::optional<MixerNodeConfig>          mixerConfig;
    std::optional<ChannelRouterNodeConfig>  channelRouterConfig;
    std::optional<DownmixNodeConfig>        downmixConfig;
    std::optional<SendNodeConfig>           sendConfig;
    std::optional<ReceiveNodeConfig>        receiveConfig;
    std::optional<SidechainInputNodeConfig> sidechainInputConfig;
    std::optional<GainNodeConfig>           gainConfig;

    // Latency reported by this node in samples (set after plugin is loaded)
    uint32_t latencySamples = 0;

    // Slicing hint — nodes with the same sliceHint are grouped into the same
    // process slice by the Default slicing strategy. Empty = "main" slice.
    std::string sliceHint;
};

/// Create a fully configured GraphNode with correct port descriptors.
/// Throws std::invalid_argument if config is missing or invalid for the type.
GraphNode createInputNode(const std::string& id, const InputNodeConfig& config);
GraphNode createOutputNode(const std::string& id, const OutputNodeConfig& config);
GraphNode createPluginNode(const std::string& id, const PluginNodeConfig& config, const ChannelLayout& ioLayout);
GraphNode createMixerNode(const std::string& id, const MixerNodeConfig& config);
GraphNode createChannelRouterNode(const std::string& id, const ChannelRouterNodeConfig& config);
GraphNode createDownmixNode(const std::string& id, const DownmixNodeConfig& config);
GraphNode createSendNode(const std::string& id, const SendNodeConfig& config);
GraphNode createReceiveNode(const std::string& id, const ReceiveNodeConfig& config);
GraphNode createSidechainInputNode(const std::string& id, const SidechainInputNodeConfig& config);
GraphNode createGainNode(const std::string& id, const GainNodeConfig& config);

} // namespace rps::coordinator
