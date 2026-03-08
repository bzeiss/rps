#pragma once

#include <rps/coordinator/AudioBuffer.hpp>
#include <rps/coordinator/Graph.hpp>

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace rps::coordinator {

// ---------------------------------------------------------------------------
// INodeProcessor — interface for nodes that process audio
// ---------------------------------------------------------------------------

/// External processing callback for PluginNodes.
/// graphExecutor provides this to let the caller inject real plugin processing.
using PluginProcessCallback = std::function<bool(
    const std::string& nodeId,         // Node id for routing
    const AudioBuffer& input,          // Deinterleaved input
    AudioBuffer& output                // Deinterleaved output (same dimensions)
)>;

// ---------------------------------------------------------------------------
// GraphExecutor — processes audio through the graph engine
// ---------------------------------------------------------------------------

/// Executes a validated graph one block at a time.
/// After prepare(), call processBlock() for each audio block.
class GraphExecutor {
public:
    GraphExecutor() = default;

    /// Prepare the executor for the given graph.
    /// Allocates all internal buffers and computes the processing order.
    /// The graph must be valid (pass Graph::validate() first).
    /// @param pluginCallback  Called for each PluginNode during processing.
    void prepare(const Graph& graph, PluginProcessCallback pluginCallback = nullptr);

    /// Process one block of audio through the graph.
    /// @param inputBuffers   Map of InputNode id → AudioBuffer with incoming audio.
    /// @param outputBuffers  Map of OutputNode id → AudioBuffer (filled by the executor).
    void processBlock(
        const std::unordered_map<std::string, AudioBuffer>& inputBuffers,
        std::unordered_map<std::string, AudioBuffer>& outputBuffers);

    /// Check if the executor is prepared.
    bool isPrepared() const { return m_prepared; }

    /// Get the processing order (topological sort result).
    const std::vector<std::string>& processingOrder() const { return m_order; }

private:
    bool m_prepared = false;
    const Graph* m_graph = nullptr;
    PluginProcessCallback m_pluginCallback;

    /// Topological processing order
    std::vector<std::string> m_order;

    /// Internal buffers: one per output port, keyed by "nodeId:portIndex"
    std::unordered_map<std::string, AudioBuffer> m_portBuffers;

    /// Scratch buffer for mixing
    AudioBuffer m_scratchBuffer;

    // Helper: get the buffer key for a port
    static std::string portKey(const std::string& nodeId, uint32_t port);

    // Node processing dispatchers
    void processInputNode(const GraphNode& node,
                          const std::unordered_map<std::string, AudioBuffer>& inputBuffers);
    void processOutputNode(const GraphNode& node,
                           std::unordered_map<std::string, AudioBuffer>& outputBuffers);
    void processGainNode(const GraphNode& node);
    void processMixerNode(const GraphNode& node);
    void processPluginNode(const GraphNode& node);
    void processChannelRouterNode(const GraphNode& node);
    void processDownmixNode(const GraphNode& node);

    /// Gather the input buffer for a node's input port (from upstream edges).
    /// For fan-in (mixer), returns the first connected buffer; mixer handles summing separately.
    AudioBuffer* getInputBuffer(const std::string& nodeId, uint32_t portIndex);
};

} // namespace rps::coordinator
