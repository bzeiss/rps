#include <rps/coordinator/GraphExecutor.hpp>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <stdexcept>

namespace rps::coordinator {

std::string GraphExecutor::portKey(const std::string& nodeId, uint32_t port) {
    return std::format("{}:{}", nodeId, port);
}

void GraphExecutor::prepare(const Graph& graph, PluginProcessCallback pluginCallback) {
    m_graph = &graph;
    m_pluginCallback = std::move(pluginCallback);
    m_portBuffers.clear();

    // Compute processing order
    m_order = graph.topologicalSort();
    if (m_order.empty() && graph.nodeCount() > 0) {
        throw std::runtime_error("Cannot prepare executor: graph has a cycle");
    }

    uint32_t blockSize = graph.config().blockSize;

    // Allocate output buffers for every output port of every node
    for (const auto& [id, node] : graph.nodes()) {
        for (const auto& port : node.outputPorts) {
            uint32_t channels = port.layout.effectiveChannelCount();
            if (channels == 0) channels = 2; // Default to stereo if unspecified
            m_portBuffers.emplace(portKey(id, port.index),
                                  AudioBuffer(channels, blockSize));
        }
    }

    m_prepared = true;
}

AudioBuffer* GraphExecutor::getInputBuffer(const std::string& nodeId, uint32_t portIndex) {
    // Find the edge that connects to this input port
    for (const auto& edge : m_graph->edges()) {
        if (edge.destNodeId == nodeId && edge.destPort == portIndex) {
            auto key = portKey(edge.sourceNodeId, edge.sourcePort);
            auto it = m_portBuffers.find(key);
            if (it != m_portBuffers.end()) {
                return &it->second;
            }
        }
    }
    return nullptr;
}

void GraphExecutor::processBlock(
    const std::unordered_map<std::string, AudioBuffer>& inputBuffers,
    std::unordered_map<std::string, AudioBuffer>& outputBuffers) {

    if (!m_prepared) {
        throw std::runtime_error("GraphExecutor::processBlock() called without prepare()");
    }

    // Clear all port buffers
    for (auto& [_, buf] : m_portBuffers) {
        buf.clear();
    }

    // Process each node in topological order
    for (const auto& nodeId : m_order) {
        const auto* node = m_graph->findNode(nodeId);
        if (!node) continue;

        switch (node->type) {
            case NodeType::Input:
            case NodeType::SidechainInput:
            case NodeType::Receive:
                processInputNode(*node, inputBuffers);
                break;

            case NodeType::Output:
            case NodeType::Send:
                processOutputNode(*node, outputBuffers);
                break;

            case NodeType::Gain:
                processGainNode(*node);
                break;

            case NodeType::Mixer:
                processMixerNode(*node);
                break;

            case NodeType::Plugin:
                processPluginNode(*node);
                break;

            case NodeType::ChannelRouter:
                processChannelRouterNode(*node);
                break;

            case NodeType::Downmix:
                processDownmixNode(*node);
                break;
        }

        // Handle fan-out: if output port has multiple consumers, each gets a copy.
        // The port buffer itself is the "master" copy. When getInputBuffer() is used
        // by downstream nodes, they all read from the same buffer. Since we process
        // in topo order, the upstream is done before any downstream starts.
        // Nodes that modify their input (like Gain) work on their own output buffer,
        // so fan-out is safe.
    }
}

void GraphExecutor::processInputNode(
    const GraphNode& node,
    const std::unordered_map<std::string, AudioBuffer>& inputBuffers) {

    // Copy external input into our output port buffer
    auto it = inputBuffers.find(node.id);
    if (it == inputBuffers.end()) return;

    auto outKey = portKey(node.id, 0);
    auto outIt = m_portBuffers.find(outKey);
    if (outIt == m_portBuffers.end()) return;

    outIt->second.copyFrom(it->second);
}

void GraphExecutor::processOutputNode(
    const GraphNode& node,
    std::unordered_map<std::string, AudioBuffer>& outputBuffers) {

    // Copy from upstream into the external output buffer
    auto* input = getInputBuffer(node.id, 0);
    if (!input) return;

    auto it = outputBuffers.find(node.id);
    if (it != outputBuffers.end()) {
        it->second.copyFrom(*input);
    } else {
        // Fallback: auto-create if caller didn't pre-allocate.
        // In the production audio path (GraphWorkerMain), outputs are always
        // pre-allocated so this branch is never taken. It exists for unit tests
        // and backwards compatibility.
        AudioBuffer copy(input->numChannels(), input->blockSize());
        copy.copyFrom(*input);
        outputBuffers.emplace(node.id, std::move(copy));
    }
}

void GraphExecutor::processGainNode(const GraphNode& node) {
    auto* input = getInputBuffer(node.id, 0);
    if (!input) return;

    auto outKey = portKey(node.id, 0);
    auto outIt = m_portBuffers.find(outKey);
    if (outIt == m_portBuffers.end()) return;

    if (node.gainConfig && node.gainConfig->bypass) {
        outIt->second.copyFrom(*input);
        return;
    }

    if (node.gainConfig && node.gainConfig->mute) {
        outIt->second.clear();
        return;
    }

    outIt->second.copyFrom(*input);
    float gain = node.gainConfig ? node.gainConfig->gain : 1.0f;
    outIt->second.applyGain(gain);
}

void GraphExecutor::processMixerNode(const GraphNode& node) {
    auto outKey = portKey(node.id, 0);
    auto outIt = m_portBuffers.find(outKey);
    if (outIt == m_portBuffers.end()) return;

    // Clear output
    outIt->second.clear();

    // Sum all inputs with 64-bit accumulation via AudioBuffer::mixIn()
    for (const auto& port : node.inputPorts) {
        auto* input = getInputBuffer(node.id, port.index);
        if (!input) continue;

        float gain = 1.0f;
        if (node.mixerConfig && port.index < node.mixerConfig->inputGains.size()) {
            gain = node.mixerConfig->inputGains[port.index];
        }
        outIt->second.mixIn(*input, gain);
    }
}

void GraphExecutor::processPluginNode(const GraphNode& node) {
    auto* input = getInputBuffer(node.id, 0);
    if (!input) return;

    auto outKey = portKey(node.id, 0);
    auto outIt = m_portBuffers.find(outKey);
    if (outIt == m_portBuffers.end()) return;

    if (m_pluginCallback) {
        bool ok = m_pluginCallback(node.id, *input, outIt->second);
        if (!ok) {
            // Fallback: pass audio through unprocessed
            outIt->second.copyFrom(*input);
        }
    } else {
        // No plugin callback — pass through
        outIt->second.copyFrom(*input);
    }
}

void GraphExecutor::processChannelRouterNode(const GraphNode& node) {
    auto* input = getInputBuffer(node.id, 0);
    if (!input) return;

    auto outKey = portKey(node.id, 0);
    auto outIt = m_portBuffers.find(outKey);
    if (outIt == m_portBuffers.end()) return;

    outIt->second.clear();

    if (!node.channelRouterConfig) {
        outIt->second.copyFrom(*input);
        return;
    }

    const auto& routing = node.channelRouterConfig->routing;
    uint32_t outChannels = outIt->second.numChannels();
    uint32_t blockSize = outIt->second.blockSize();

    for (uint32_t outCh = 0; outCh < outChannels && outCh < routing.size(); ++outCh) {
        uint32_t srcCh = routing[outCh];
        if (srcCh < input->numChannels()) {
            const float* src = input->channel(srcCh);
            float* dst = outIt->second.channel(outCh);
            std::copy(src, src + blockSize, dst);
        }
    }
}

void GraphExecutor::processDownmixNode(const GraphNode& node) {
    auto* input = getInputBuffer(node.id, 0);
    if (!input) return;

    auto outKey = portKey(node.id, 0);
    auto outIt = m_portBuffers.find(outKey);
    if (outIt == m_portBuffers.end()) return;

    outIt->second.clear();

    // Simple downmix: for now, use equal-power fold-down.
    // Each output channel gets contributions from input channels.
    // This is a simplified version; a proper implementation would use
    // ITU-R BS.775 coefficients based on the specific format pair.
    uint32_t inCh = input->numChannels();
    uint32_t outCh = outIt->second.numChannels();
    uint32_t blockSize = input->blockSize();

    if (outCh == 2 && inCh > 2) {
        // Stereo downmix: simple fold-down
        // L_out = sum of all odd-indexed (left-side) input channels
        // R_out = sum of all even-indexed (right-side) input channels
        // This is a rough approximation; proper ITU coefficients are format-specific.
        float scale = 1.0f / static_cast<float>(std::max(1u, (inCh + 1) / 2));
        float* outL = outIt->second.channel(0);
        float* outR = outIt->second.channel(1);

        for (uint32_t ch = 0; ch < inCh; ++ch) {
            const float* src = input->channel(ch);
            float* dst = (ch % 2 == 0) ? outL : outR;
            for (uint32_t s = 0; s < blockSize; ++s) {
                // 64-bit accumulation
                double acc = static_cast<double>(dst[s])
                           + static_cast<double>(src[s]) * static_cast<double>(scale);
                dst[s] = static_cast<float>(acc);
            }
        }
    } else {
        // Generic: map first N channels directly, drop the rest
        uint32_t copy = std::min(inCh, outCh);
        for (uint32_t ch = 0; ch < copy; ++ch) {
            std::copy(input->channel(ch), input->channel(ch) + blockSize,
                      outIt->second.channel(ch));
        }
    }
}

} // namespace rps::coordinator
