#pragma once

#include <rps/coordinator/Graph.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>

namespace rps::coordinator {

// ---------------------------------------------------------------------------
// Latency computation over the processing graph
// ---------------------------------------------------------------------------

/// Compute the longest-path latency (in samples) from each source node
/// to each sink node. This is the sum of node latencies along the critical path.
class LatencyCalculator {
public:
    /// Compute latencies for the given graph. Requires a valid DAG (no cycles).
    /// Returns a map of outputNodeId → total latency in samples.
    static std::unordered_map<std::string, uint32_t> compute(const Graph& graph);

    /// Compute the latency for a specific output node.
    /// Returns 0 if the node is not an output or the graph has cycles.
    static uint32_t computeForOutput(const Graph& graph, const std::string& outputNodeId);
};

} // namespace rps::coordinator
