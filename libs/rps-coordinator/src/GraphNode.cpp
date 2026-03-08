#include <rps/coordinator/GraphNode.hpp>

#include <stdexcept>
#include <array>

namespace rps::coordinator {

namespace {

struct NodeTypeEntry {
    NodeType         type;
    std::string_view name;
};

constexpr std::array kNodeTypeTable = {
    NodeTypeEntry{NodeType::Input,          "input"},
    NodeTypeEntry{NodeType::Output,         "output"},
    NodeTypeEntry{NodeType::Plugin,         "plugin"},
    NodeTypeEntry{NodeType::Mixer,          "mixer"},
    NodeTypeEntry{NodeType::ChannelRouter,  "channel_router"},
    NodeTypeEntry{NodeType::Downmix,        "downmix"},
    NodeTypeEntry{NodeType::Send,           "send"},
    NodeTypeEntry{NodeType::Receive,        "receive"},
    NodeTypeEntry{NodeType::SidechainInput, "sidechain_input"},
    NodeTypeEntry{NodeType::Gain,           "gain"},
};

PortDescriptor makePort(uint32_t index, PortDirection dir, const ChannelLayout& layout) {
    return PortDescriptor{index, dir, layout};
}

} // anonymous namespace

std::string_view nodeTypeToString(NodeType type) {
    for (const auto& e : kNodeTypeTable) {
        if (e.type == type) return e.name;
    }
    return "unknown";
}

std::optional<NodeType> nodeTypeFromString(std::string_view name) {
    for (const auto& e : kNodeTypeTable) {
        if (e.name == name) return e.type;
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Factory functions
// ---------------------------------------------------------------------------

GraphNode createInputNode(const std::string& id, const InputNodeConfig& config) {
    GraphNode node;
    node.id = id;
    node.type = NodeType::Input;
    node.inputConfig = config;
    // Input nodes have no input ports, one output port
    node.outputPorts.push_back(makePort(0, PortDirection::Output, config.layout));
    return node;
}

GraphNode createOutputNode(const std::string& id, const OutputNodeConfig& config) {
    GraphNode node;
    node.id = id;
    node.type = NodeType::Output;
    node.outputConfig = config;
    // Output nodes have one input port, no output ports
    node.inputPorts.push_back(makePort(0, PortDirection::Input, config.layout));
    return node;
}

GraphNode createPluginNode(const std::string& id, const PluginNodeConfig& config,
                           const ChannelLayout& ioLayout) {
    GraphNode node;
    node.id = id;
    node.type = NodeType::Plugin;
    node.pluginConfig = config;
    // Plugin nodes: 1 input, 1 output (same layout for now; may differ after negotiation)
    node.inputPorts.push_back(makePort(0, PortDirection::Input, ioLayout));
    node.outputPorts.push_back(makePort(0, PortDirection::Output, ioLayout));
    return node;
}

GraphNode createMixerNode(const std::string& id, const MixerNodeConfig& config) {
    GraphNode node;
    node.id = id;
    node.type = NodeType::Mixer;
    node.mixerConfig = config;
    // K input ports, 1 output port
    for (uint32_t i = 0; i < config.numInputs; ++i) {
        node.inputPorts.push_back(makePort(i, PortDirection::Input, config.outputLayout));
    }
    node.outputPorts.push_back(makePort(0, PortDirection::Output, config.outputLayout));

    // Fill default gains if not provided
    if (node.mixerConfig->inputGains.empty()) {
        node.mixerConfig->inputGains.assign(config.numInputs, 1.0f);
    }
    return node;
}

GraphNode createChannelRouterNode(const std::string& id,
                                   const ChannelRouterNodeConfig& config) {
    if (config.routing.size() != config.outputLayout.effectiveChannelCount()) {
        throw std::invalid_argument(
            "ChannelRouterNode routing table size must match output channel count");
    }
    GraphNode node;
    node.id = id;
    node.type = NodeType::ChannelRouter;
    node.channelRouterConfig = config;
    node.inputPorts.push_back(makePort(0, PortDirection::Input, config.inputLayout));
    node.outputPorts.push_back(makePort(0, PortDirection::Output, config.outputLayout));
    return node;
}

GraphNode createDownmixNode(const std::string& id, const DownmixNodeConfig& config) {
    if (config.outputLayout.effectiveChannelCount() >= config.inputLayout.effectiveChannelCount()) {
        throw std::invalid_argument("DownmixNode output must have fewer channels than input");
    }
    GraphNode node;
    node.id = id;
    node.type = NodeType::Downmix;
    node.downmixConfig = config;
    node.inputPorts.push_back(makePort(0, PortDirection::Input, config.inputLayout));
    node.outputPorts.push_back(makePort(0, PortDirection::Output, config.outputLayout));
    return node;
}

GraphNode createSendNode(const std::string& id, const SendNodeConfig& config) {
    GraphNode node;
    node.id = id;
    node.type = NodeType::Send;
    node.sendConfig = config;
    node.inputPorts.push_back(makePort(0, PortDirection::Input, config.layout));
    return node;
}

GraphNode createReceiveNode(const std::string& id, const ReceiveNodeConfig& config) {
    GraphNode node;
    node.id = id;
    node.type = NodeType::Receive;
    node.receiveConfig = config;
    node.outputPorts.push_back(makePort(0, PortDirection::Output, config.layout));
    return node;
}

GraphNode createSidechainInputNode(const std::string& id,
                                    const SidechainInputNodeConfig& config) {
    GraphNode node;
    node.id = id;
    node.type = NodeType::SidechainInput;
    node.sidechainInputConfig = config;
    node.outputPorts.push_back(makePort(0, PortDirection::Output, config.layout));
    return node;
}

GraphNode createGainNode(const std::string& id, const GainNodeConfig& config) {
    GraphNode node;
    node.id = id;
    node.type = NodeType::Gain;
    node.gainConfig = config;
    node.inputPorts.push_back(makePort(0, PortDirection::Input, config.layout));
    node.outputPorts.push_back(makePort(0, PortDirection::Output, config.layout));
    return node;
}

} // namespace rps::coordinator
