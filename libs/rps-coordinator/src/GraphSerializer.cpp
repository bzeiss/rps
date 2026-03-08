#include <rps/coordinator/GraphSerializer.hpp>
#include <rps/coordinator/ChannelFormat.hpp>
#include <rps/coordinator/GraphNode.hpp>

#include <boost/json.hpp>
#include <format>
#include <stdexcept>

namespace json = boost::json;

namespace rps::coordinator {

namespace {

// ---------------------------------------------------------------------------
// Helpers: ChannelLayout ↔ JSON
// ---------------------------------------------------------------------------

json::object layoutToJson(const ChannelLayout& l) {
    json::object obj;
    obj["format"] = std::string(channelFormatToString(l.format));
    obj["channelCount"] = l.channelCount;
    return obj;
}

ChannelLayout layoutFromJson(const json::object& obj) {
    ChannelLayout l;
    if (obj.contains("format")) {
        l.format = channelFormatFromString(json::value_to<std::string>(obj.at("format")));
    }
    if (obj.contains("channelCount")) {
        l.channelCount = static_cast<uint32_t>(obj.at("channelCount").as_int64());
    }
    // If format is named, sync channelCount
    uint32_t named = getChannelCount(l.format);
    if (named > 0 && l.channelCount == 0) {
        l.channelCount = named;
    }
    return l;
}

// ---------------------------------------------------------------------------
// Node → JSON
// ---------------------------------------------------------------------------

json::object nodeToJson(const GraphNode& node) {
    json::object obj;
    obj["id"] = node.id;
    obj["type"] = std::string(nodeTypeToString(node.type));
    obj["latencySamples"] = node.latencySamples;

    if (node.linkedNodeId) {
        obj["linkedNodeId"] = *node.linkedNodeId;
    }

    if (!node.sliceHint.empty()) {
        obj["sliceHint"] = node.sliceHint;
    }

    json::object config;
    switch (node.type) {
        case NodeType::Input:
            if (node.inputConfig) {
                config["layout"] = layoutToJson(node.inputConfig->layout);
                config["shmName"] = node.inputConfig->shmName;
            }
            break;
        case NodeType::Output:
            if (node.outputConfig) {
                config["layout"] = layoutToJson(node.outputConfig->layout);
                config["shmName"] = node.outputConfig->shmName;
            }
            break;
        case NodeType::Plugin:
            if (node.pluginConfig) {
                config["pluginPath"] = node.pluginConfig->pluginPath;
                config["format"] = node.pluginConfig->format;
                // Store the I/O layout from the first input port
                if (!node.inputPorts.empty()) {
                    config["ioLayout"] = layoutToJson(node.inputPorts[0].layout);
                }
            }
            break;
        case NodeType::Mixer:
            if (node.mixerConfig) {
                config["outputLayout"] = layoutToJson(node.mixerConfig->outputLayout);
                config["numInputs"] = node.mixerConfig->numInputs;
                json::array gains;
                for (float g : node.mixerConfig->inputGains) {
                    gains.push_back(g);
                }
                config["inputGains"] = std::move(gains);
            }
            break;
        case NodeType::ChannelRouter:
            if (node.channelRouterConfig) {
                config["inputLayout"] = layoutToJson(node.channelRouterConfig->inputLayout);
                config["outputLayout"] = layoutToJson(node.channelRouterConfig->outputLayout);
                json::array routing;
                for (uint32_t r : node.channelRouterConfig->routing) {
                    routing.push_back(static_cast<int64_t>(r));
                }
                config["routing"] = std::move(routing);
            }
            break;
        case NodeType::Downmix:
            if (node.downmixConfig) {
                config["inputLayout"] = layoutToJson(node.downmixConfig->inputLayout);
                config["outputLayout"] = layoutToJson(node.downmixConfig->outputLayout);
            }
            break;
        case NodeType::Send:
            if (node.sendConfig) {
                config["layout"] = layoutToJson(node.sendConfig->layout);
                config["shmName"] = node.sendConfig->shmName;
            }
            break;
        case NodeType::Receive:
            if (node.receiveConfig) {
                config["layout"] = layoutToJson(node.receiveConfig->layout);
                config["shmName"] = node.receiveConfig->shmName;
            }
            break;
        case NodeType::SidechainInput:
            if (node.sidechainInputConfig) {
                config["layout"] = layoutToJson(node.sidechainInputConfig->layout);
                config["shmName"] = node.sidechainInputConfig->shmName;
            }
            break;
        case NodeType::Gain:
            if (node.gainConfig) {
                config["layout"] = layoutToJson(node.gainConfig->layout);
                config["gain"] = static_cast<double>(node.gainConfig->gain);
                config["mute"] = node.gainConfig->mute;
                config["bypass"] = node.gainConfig->bypass;
            }
            break;
    }
    obj["config"] = std::move(config);
    return obj;
}

// ---------------------------------------------------------------------------
// JSON → Node
// ---------------------------------------------------------------------------

GraphNode nodeFromJson(const json::object& obj) {
    auto id = json::value_to<std::string>(obj.at("id"));
    auto typeStr = json::value_to<std::string>(obj.at("type"));
    auto typeOpt = nodeTypeFromString(typeStr);
    if (!typeOpt) {
        throw std::runtime_error(std::format("Unknown node type: '{}'", typeStr));
    }

    const auto& config = obj.at("config").as_object();

    switch (*typeOpt) {
        case NodeType::Input: {
            InputNodeConfig c;
            if (config.contains("layout")) c.layout = layoutFromJson(config.at("layout").as_object());
            if (config.contains("shmName")) c.shmName = json::value_to<std::string>(config.at("shmName"));
            auto node = createInputNode(id, c);
            if (obj.contains("latencySamples")) node.latencySamples = static_cast<uint32_t>(obj.at("latencySamples").as_int64());
            if (obj.contains("linkedNodeId")) node.linkedNodeId = json::value_to<std::string>(obj.at("linkedNodeId"));
            if (obj.contains("sliceHint")) node.sliceHint = json::value_to<std::string>(obj.at("sliceHint"));
            return node;
        }
        case NodeType::Output: {
            OutputNodeConfig c;
            if (config.contains("layout")) c.layout = layoutFromJson(config.at("layout").as_object());
            if (config.contains("shmName")) c.shmName = json::value_to<std::string>(config.at("shmName"));
            auto node = createOutputNode(id, c);
            if (obj.contains("latencySamples")) node.latencySamples = static_cast<uint32_t>(obj.at("latencySamples").as_int64());
            if (obj.contains("linkedNodeId")) node.linkedNodeId = json::value_to<std::string>(obj.at("linkedNodeId"));
            if (obj.contains("sliceHint")) node.sliceHint = json::value_to<std::string>(obj.at("sliceHint"));
            return node;
        }
        case NodeType::Plugin: {
            PluginNodeConfig c;
            if (config.contains("pluginPath")) c.pluginPath = json::value_to<std::string>(config.at("pluginPath"));
            if (config.contains("format")) c.format = json::value_to<std::string>(config.at("format"));
            ChannelLayout ioLayout;
            if (config.contains("ioLayout")) ioLayout = layoutFromJson(config.at("ioLayout").as_object());
            auto node = createPluginNode(id, c, ioLayout);
            if (obj.contains("latencySamples")) node.latencySamples = static_cast<uint32_t>(obj.at("latencySamples").as_int64());
            if (obj.contains("linkedNodeId")) node.linkedNodeId = json::value_to<std::string>(obj.at("linkedNodeId"));
            if (obj.contains("sliceHint")) node.sliceHint = json::value_to<std::string>(obj.at("sliceHint"));
            return node;
        }
        case NodeType::Mixer: {
            MixerNodeConfig c;
            if (config.contains("outputLayout")) c.outputLayout = layoutFromJson(config.at("outputLayout").as_object());
            if (config.contains("numInputs")) c.numInputs = static_cast<uint32_t>(config.at("numInputs").as_int64());
            if (config.contains("inputGains")) {
                for (const auto& v : config.at("inputGains").as_array()) {
                    c.inputGains.push_back(static_cast<float>(v.as_double()));
                }
            }
            auto node = createMixerNode(id, c);
            if (obj.contains("latencySamples")) node.latencySamples = static_cast<uint32_t>(obj.at("latencySamples").as_int64());
            if (obj.contains("linkedNodeId")) node.linkedNodeId = json::value_to<std::string>(obj.at("linkedNodeId"));
            if (obj.contains("sliceHint")) node.sliceHint = json::value_to<std::string>(obj.at("sliceHint"));
            return node;
        }
        case NodeType::ChannelRouter: {
            ChannelRouterNodeConfig c;
            if (config.contains("inputLayout")) c.inputLayout = layoutFromJson(config.at("inputLayout").as_object());
            if (config.contains("outputLayout")) c.outputLayout = layoutFromJson(config.at("outputLayout").as_object());
            if (config.contains("routing")) {
                for (const auto& v : config.at("routing").as_array()) {
                    c.routing.push_back(static_cast<uint32_t>(v.as_int64()));
                }
            }
            auto node = createChannelRouterNode(id, c);
            if (obj.contains("latencySamples")) node.latencySamples = static_cast<uint32_t>(obj.at("latencySamples").as_int64());
            if (obj.contains("linkedNodeId")) node.linkedNodeId = json::value_to<std::string>(obj.at("linkedNodeId"));
            if (obj.contains("sliceHint")) node.sliceHint = json::value_to<std::string>(obj.at("sliceHint"));
            return node;
        }
        case NodeType::Downmix: {
            DownmixNodeConfig c;
            if (config.contains("inputLayout")) c.inputLayout = layoutFromJson(config.at("inputLayout").as_object());
            if (config.contains("outputLayout")) c.outputLayout = layoutFromJson(config.at("outputLayout").as_object());
            auto node = createDownmixNode(id, c);
            if (obj.contains("latencySamples")) node.latencySamples = static_cast<uint32_t>(obj.at("latencySamples").as_int64());
            if (obj.contains("linkedNodeId")) node.linkedNodeId = json::value_to<std::string>(obj.at("linkedNodeId"));
            if (obj.contains("sliceHint")) node.sliceHint = json::value_to<std::string>(obj.at("sliceHint"));
            return node;
        }
        case NodeType::Send: {
            SendNodeConfig c;
            if (config.contains("layout")) c.layout = layoutFromJson(config.at("layout").as_object());
            if (config.contains("shmName")) c.shmName = json::value_to<std::string>(config.at("shmName"));
            auto node = createSendNode(id, c);
            if (obj.contains("latencySamples")) node.latencySamples = static_cast<uint32_t>(obj.at("latencySamples").as_int64());
            if (obj.contains("linkedNodeId")) node.linkedNodeId = json::value_to<std::string>(obj.at("linkedNodeId"));
            if (obj.contains("sliceHint")) node.sliceHint = json::value_to<std::string>(obj.at("sliceHint"));
            return node;
        }
        case NodeType::Receive: {
            ReceiveNodeConfig c;
            if (config.contains("layout")) c.layout = layoutFromJson(config.at("layout").as_object());
            if (config.contains("shmName")) c.shmName = json::value_to<std::string>(config.at("shmName"));
            auto node = createReceiveNode(id, c);
            if (obj.contains("latencySamples")) node.latencySamples = static_cast<uint32_t>(obj.at("latencySamples").as_int64());
            if (obj.contains("linkedNodeId")) node.linkedNodeId = json::value_to<std::string>(obj.at("linkedNodeId"));
            if (obj.contains("sliceHint")) node.sliceHint = json::value_to<std::string>(obj.at("sliceHint"));
            return node;
        }
        case NodeType::SidechainInput: {
            SidechainInputNodeConfig c;
            if (config.contains("layout")) c.layout = layoutFromJson(config.at("layout").as_object());
            if (config.contains("shmName")) c.shmName = json::value_to<std::string>(config.at("shmName"));
            auto node = createSidechainInputNode(id, c);
            if (obj.contains("latencySamples")) node.latencySamples = static_cast<uint32_t>(obj.at("latencySamples").as_int64());
            if (obj.contains("linkedNodeId")) node.linkedNodeId = json::value_to<std::string>(obj.at("linkedNodeId"));
            if (obj.contains("sliceHint")) node.sliceHint = json::value_to<std::string>(obj.at("sliceHint"));
            return node;
        }
        case NodeType::Gain: {
            GainNodeConfig c;
            if (config.contains("layout")) c.layout = layoutFromJson(config.at("layout").as_object());
            if (config.contains("gain")) c.gain = static_cast<float>(config.at("gain").as_double());
            if (config.contains("mute")) c.mute = config.at("mute").as_bool();
            if (config.contains("bypass")) c.bypass = config.at("bypass").as_bool();
            auto node = createGainNode(id, c);
            if (obj.contains("latencySamples")) node.latencySamples = static_cast<uint32_t>(obj.at("latencySamples").as_int64());
            if (obj.contains("linkedNodeId")) node.linkedNodeId = json::value_to<std::string>(obj.at("linkedNodeId"));
            if (obj.contains("sliceHint")) node.sliceHint = json::value_to<std::string>(obj.at("sliceHint"));
            return node;
        }
    }
    throw std::runtime_error(std::format("Unhandled node type: '{}'", typeStr));
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

std::string GraphSerializer::toJson(const Graph& graph) {
    json::object root;
    root["version"] = 1;

    json::object graphObj;
    graphObj["sampleRate"] = graph.config().sampleRate;
    graphObj["blockSize"] = graph.config().blockSize;

    // Nodes
    json::array nodesArr;
    for (const auto& [_, node] : graph.nodes()) {
        nodesArr.push_back(nodeToJson(node));
    }
    graphObj["nodes"] = std::move(nodesArr);

    // Edges
    json::array edgesArr;
    for (const auto& edge : graph.edges()) {
        json::object edgeObj;
        edgeObj["id"] = edge.id;
        edgeObj["sourceNodeId"] = edge.sourceNodeId;
        edgeObj["sourcePort"] = static_cast<int64_t>(edge.sourcePort);
        edgeObj["destNodeId"] = edge.destNodeId;
        edgeObj["destPort"] = static_cast<int64_t>(edge.destPort);
        edgesArr.push_back(std::move(edgeObj));
    }
    graphObj["edges"] = std::move(edgesArr);

    root["graph"] = std::move(graphObj);
    return json::serialize(root);
}

Graph GraphSerializer::fromJson(const std::string& jsonStr) {
    auto val = json::parse(jsonStr);
    const auto& root = val.as_object();

    if (!root.contains("graph")) {
        throw std::runtime_error("JSON missing 'graph' key");
    }
    const auto& graphObj = root.at("graph").as_object();

    Graph::Config config;
    if (graphObj.contains("sampleRate")) {
        config.sampleRate = static_cast<uint32_t>(graphObj.at("sampleRate").as_int64());
    }
    if (graphObj.contains("blockSize")) {
        config.blockSize = static_cast<uint32_t>(graphObj.at("blockSize").as_int64());
    }

    Graph graph(config);

    // Nodes
    if (graphObj.contains("nodes")) {
        for (const auto& nodeVal : graphObj.at("nodes").as_array()) {
            graph.addNode(nodeFromJson(nodeVal.as_object()));
        }
    }

    // Edges
    if (graphObj.contains("edges")) {
        for (const auto& edgeVal : graphObj.at("edges").as_array()) {
            const auto& edgeObj = edgeVal.as_object();
            auto srcId = json::value_to<std::string>(edgeObj.at("sourceNodeId"));
            auto srcPort = static_cast<uint32_t>(edgeObj.at("sourcePort").as_int64());
            auto dstId = json::value_to<std::string>(edgeObj.at("destNodeId"));
            auto dstPort = static_cast<uint32_t>(edgeObj.at("destPort").as_int64());
            graph.addEdge(srcId, srcPort, dstId, dstPort);
        }
    }

    return graph;
}

} // namespace rps::coordinator
