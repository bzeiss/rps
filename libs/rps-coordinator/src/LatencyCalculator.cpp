#include <rps/coordinator/LatencyCalculator.hpp>

#include <algorithm>

namespace rps::coordinator {

std::unordered_map<std::string, uint32_t> LatencyCalculator::compute(const Graph& graph) {
    auto order = graph.topologicalSort();
    if (order.empty() && graph.nodeCount() > 0) {
        return {};  // Cycle detected — cannot compute latencies
    }

    // For each node, compute the longest path (max cumulative latency) from any source.
    std::unordered_map<std::string, uint32_t> maxLatency;
    for (const auto& nodeId : order) {
        maxLatency[nodeId] = 0;
    }

    // Forward pass through topological order
    for (const auto& nodeId : order) {
        const auto* node = graph.findNode(nodeId);
        if (!node) continue;

        uint32_t currentLatency = maxLatency[nodeId] + node->latencySamples;

        // Propagate to successors
        for (const auto* edge : graph.edgesFrom(nodeId)) {
            maxLatency[edge->destNodeId] =
                std::max(maxLatency[edge->destNodeId], currentLatency);
        }
    }

    // Collect results for output/send nodes only
    std::unordered_map<std::string, uint32_t> result;
    for (const auto& [id, node] : graph.nodes()) {
        if (node.type == NodeType::Output || node.type == NodeType::Send) {
            result[id] = maxLatency[id] + node.latencySamples;
        }
    }
    return result;
}

uint32_t LatencyCalculator::computeForOutput(const Graph& graph,
                                              const std::string& outputNodeId) {
    auto all = compute(graph);
    auto it = all.find(outputNodeId);
    return (it != all.end()) ? it->second : 0;
}

} // namespace rps::coordinator
