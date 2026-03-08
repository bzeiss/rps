#include <rps/coordinator/GraphSlicer.hpp>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <format>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace rps::coordinator {

namespace {

// Assign a unique slice index to each node based on the strategy.
// Returns: nodeId → sliceIndex
std::unordered_map<std::string, uint32_t> assignSlices(
    const Graph& graph, SlicingStrategy strategy) {

    std::unordered_map<std::string, uint32_t> assignment;

    if (strategy == SlicingStrategy::Performance) {
        // Everything in slice 0
        for (const auto& [id, _] : graph.nodes()) {
            assignment[id] = 0;
        }
        return assignment;
    }

    if (strategy == SlicingStrategy::CrashIsolation) {
        // Each PluginNode gets its own slice. Non-plugin nodes are assigned
        // to the slice of their first upstream plugin, or slice 0 if none.
        uint32_t nextSlice = 0;
        std::unordered_map<std::string, uint32_t> pluginSlices;

        for (const auto& [id, node] : graph.nodes()) {
            if (node.type == NodeType::Plugin) {
                pluginSlices[id] = nextSlice++;
            }
        }

        if (pluginSlices.empty()) {
            // No plugins — everything in one slice
            for (const auto& [id, _] : graph.nodes()) {
                assignment[id] = 0;
            }
            return assignment;
        }

        // Assign plugins to their own slices
        for (const auto& [id, sliceIdx] : pluginSlices) {
            assignment[id] = sliceIdx;
        }

        // For non-plugin nodes, walk upstream edges to find the nearest plugin.
        // If a node feeds into multiple plugins (fan-out), assign it to the
        // first one encountered. Input nodes and nodes before any plugin get
        // assigned to the first slice that consumes them.
        auto topoOrder = graph.topologicalSort();
        for (const auto& nodeId : topoOrder) {
            if (assignment.contains(nodeId)) continue;

            // Look at downstream edges to find which slice consumes this node
            auto outEdges = graph.edgesFrom(nodeId);
            for (const auto* e : outEdges) {
                if (assignment.contains(e->destNodeId)) {
                    assignment[nodeId] = assignment[e->destNodeId];
                    break;
                }
            }
            // Fallback: check upstream edges (for sink nodes like OutputNode)
            if (!assignment.contains(nodeId)) {
                auto inEdges = graph.edgesTo(nodeId);
                for (const auto* e : inEdges) {
                    if (assignment.contains(e->sourceNodeId)) {
                        assignment[nodeId] = assignment[e->sourceNodeId];
                        break;
                    }
                }
            }
            // If still unassigned (input nodes or orphans), assign to slice 0
            if (!assignment.contains(nodeId)) {
                assignment[nodeId] = 0;
            }
        }

        return assignment;
    }

    // Default strategy: group by sliceHint
    {
        std::unordered_map<std::string, uint32_t> hintToSlice;
        uint32_t nextSlice = 0;
        hintToSlice[""] = nextSlice++; // Empty hint = "main" slice (0)

        // First pass: assign PluginNodes by their sliceHint
        for (const auto& [id, node] : graph.nodes()) {
            if (node.type == NodeType::Plugin) {
                const std::string& hint = node.sliceHint;
                if (!hintToSlice.contains(hint)) {
                    hintToSlice[hint] = nextSlice++;
                }
                assignment[id] = hintToSlice[hint];
            }
        }

        // Second pass: assign non-plugin nodes to the slice of their
        // first downstream consumer, or upstream source (topological order)
        auto topoOrder = graph.topologicalSort();
        for (const auto& nodeId : topoOrder) {
            if (assignment.contains(nodeId)) continue;

            // Try downstream first
            auto outEdges = graph.edgesFrom(nodeId);
            for (const auto* e : outEdges) {
                if (assignment.contains(e->destNodeId)) {
                    assignment[nodeId] = assignment[e->destNodeId];
                    break;
                }
            }
            // Fallback: check upstream edges (for sink nodes like OutputNode)
            if (!assignment.contains(nodeId)) {
                auto inEdges = graph.edgesTo(nodeId);
                for (const auto* e : inEdges) {
                    if (assignment.contains(e->sourceNodeId)) {
                        assignment[nodeId] = assignment[e->sourceNodeId];
                        break;
                    }
                }
            }
            if (!assignment.contains(nodeId)) {
                assignment[nodeId] = 0; // Default to main slice
            }
        }

        return assignment;
    }
}

// Compact slice indices so they're contiguous starting from 0.
// Returns the number of distinct slices.
uint32_t compactSliceIndices(std::unordered_map<std::string, uint32_t>& assignment) {
    std::set<uint32_t> usedIndices;
    for (const auto& [_, idx] : assignment) {
        usedIndices.insert(idx);
    }

    std::unordered_map<uint32_t, uint32_t> remap;
    uint32_t next = 0;
    for (uint32_t idx : usedIndices) {
        remap[idx] = next++;
    }

    for (auto& [_, idx] : assignment) {
        idx = remap[idx];
    }

    return next;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

SliceResult sliceGraph(const Graph& graph, SlicingStrategy strategy) {
    SliceResult result;

    // Performance strategy: return the original graph as-is
    if (strategy == SlicingStrategy::Performance) {
        result.slices.push_back(graph); // copy
        return result;
    }

    // Assign each node to a slice
    auto assignment = assignSlices(graph, strategy);
    uint32_t sliceCount = compactSliceIndices(assignment);

    // If everything ended up in one slice, return original
    if (sliceCount <= 1) {
        result.slices.push_back(graph);
        return result;
    }

    spdlog::debug("GraphSlicer: partitioning into {} slices", sliceCount);

    // Create empty subgraphs with the same config
    result.slices.resize(sliceCount);
    for (auto& slice : result.slices) {
        slice.setConfig(graph.config());
    }

    // Add nodes to their respective slices
    for (const auto& [id, node] : graph.nodes()) {
        uint32_t sliceIdx = assignment.at(id);
        result.slices[sliceIdx].addNode(node);
    }

    // Process edges: internal edges stay, cross-slice edges get Send/Receive bridges
    uint32_t bridgeCounter = 0;
    for (const auto& edge : graph.edges()) {
        uint32_t srcSlice = assignment.at(edge.sourceNodeId);
        uint32_t dstSlice = assignment.at(edge.destNodeId);

        if (srcSlice == dstSlice) {
            // Internal edge — add directly to the slice
            result.slices[srcSlice].addEdge(
                edge.sourceNodeId, edge.sourcePort,
                edge.destNodeId, edge.destPort);
        } else {
            // Cross-slice edge — insert Send/Receive bridge
            std::string shmName = std::format("rps_bridge_{}", bridgeCounter++);
            std::string sendId = std::format("__send_{}_{}", edge.sourceNodeId, bridgeCounter);
            std::string recvId = std::format("__recv_{}_{}", edge.destNodeId, bridgeCounter);

            // Determine channel layout from the source node's output port
            ChannelLayout bridgeLayout = {ChannelFormat::Stereo, 2}; // default
            const auto* srcNode = graph.findNode(edge.sourceNodeId);
            if (srcNode && edge.sourcePort < srcNode->outputPorts.size()) {
                bridgeLayout = srcNode->outputPorts[edge.sourcePort].layout;
            }

            // Create SendNode in source slice
            auto sendNode = createSendNode(sendId, {bridgeLayout, shmName});
            result.slices[srcSlice].addNode(std::move(sendNode));
            result.slices[srcSlice].addEdge(
                edge.sourceNodeId, edge.sourcePort,
                sendId, 0);

            // Create ReceiveNode in destination slice
            auto recvNode = createReceiveNode(recvId, {bridgeLayout, shmName});
            result.slices[dstSlice].addNode(std::move(recvNode));
            result.slices[dstSlice].addEdge(
                recvId, 0,
                edge.destNodeId, edge.destPort);

            // Record the bridge
            ShmBridge bridge;
            bridge.sendNodeId = sendId;
            bridge.receiveNodeId = recvId;
            bridge.shmName = shmName;
            bridge.sourceSlice = srcSlice;
            bridge.destSlice = dstSlice;
            result.bridges.push_back(std::move(bridge));
        }
    }

    spdlog::debug("GraphSlicer: {} bridges inserted", result.bridges.size());

    return result;
}

} // namespace rps::coordinator
