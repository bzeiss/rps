#pragma once

#include <rps/coordinator/Graph.hpp>
#include <rps/coordinator/GraphExecutor.hpp>
#include <rps/coordinator/GraphSlicer.hpp>
#include <rps/coordinator/ProcessSlice.hpp>
#include <rps/audio/SharedAudioRing.hpp>

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace rps::coordinator {

// ---------------------------------------------------------------------------
// Graph lifecycle state
// ---------------------------------------------------------------------------

enum class GraphState : uint8_t {
    Inactive,    ///< Graph exists but is not processing audio
    Active,      ///< Graph is processing audio (slices are running)
};

// ---------------------------------------------------------------------------
// Graph info (queryable summary)
// ---------------------------------------------------------------------------

struct GraphInfo {
    std::string       graphId;
    GraphState        state = GraphState::Inactive;
    uint32_t          nodeCount = 0;
    uint32_t          edgeCount = 0;
    uint32_t          sliceCount = 0;
    SlicingStrategy   strategy = SlicingStrategy::Performance;
};

// ---------------------------------------------------------------------------
// Coordinator — top-level graph orchestrator
// ---------------------------------------------------------------------------

/// Manages one or more audio processing graphs. Each graph can be partitioned
/// into slices (subgraphs) running in separate rps-pluginhost processes.
///
/// Thread safety: all public methods are mutex-protected. The audio processing
/// path (processBlock) is NOT mutex-protected — it is called from the audio
/// thread and operates on pre-prepared immutable state.
class Coordinator {
public:
    Coordinator() = default;
    ~Coordinator();

    // Non-copyable
    Coordinator(const Coordinator&) = delete;
    Coordinator& operator=(const Coordinator&) = delete;

    /// Set the path to the rps-pluginhost binary (required for multi-slice mode).
    void setHostBinaryPath(const std::string& path) { m_hostBinaryPath = path; }

    // -- Graph construction --

    /// Create a new graph and return its unique ID.
    std::string createGraph(const Graph::Config& config);

    /// Destroy a graph and all its resources. Deactivates first if active.
    void destroyGraph(const std::string& graphId);

    // -- Node management --

    /// Add a node to a graph. Returns the node ID.
    std::string addNode(const std::string& graphId, GraphNode node);

    /// Remove a node and its connected edges.
    void removeNode(const std::string& graphId, const std::string& nodeId);

    // -- Edge management --

    /// Connect two nodes. Returns the edge ID.
    std::string connectNodes(const std::string& graphId,
                             const std::string& sourceNodeId, uint32_t sourcePort,
                             const std::string& destNodeId, uint32_t destPort);

    /// Remove an edge.
    void disconnectNodes(const std::string& graphId, const std::string& edgeId);

    // -- Lifecycle --

    /// Validate the graph. Returns validation result.
    ValidationResult validateGraph(const std::string& graphId);

    /// Activate the graph: slice it, spawn processes, start audio.
    /// @param strategy  How to partition the graph into slices.
    /// @param pluginCallback  Callback for processing PluginNodes (in-process mode).
    void activateGraph(const std::string& graphId,
                       SlicingStrategy strategy = SlicingStrategy::Performance,
                       PluginProcessCallback pluginCallback = nullptr);

    /// Deactivate the graph: stop audio, terminate processes.
    void deactivateGraph(const std::string& graphId);

    // -- Queries --

    /// Get summary info about a graph.
    GraphInfo getGraphInfo(const std::string& graphId);

    /// Get the latency in samples for a specific output node.
    uint32_t getOutputLatency(const std::string& graphId,
                              const std::string& outputNodeId);

    // -- State persistence --

    /// Serialize graph to JSON string.
    std::string serializeGraph(const std::string& graphId);

    /// Deserialize graph from JSON. Returns new graph ID.
    std::string deserializeGraph(const std::string& jsonData);

    // -- Direct access (for in-process use) --

    /// Get the graph for direct manipulation (not thread-safe with activate).
    Graph* getGraph(const std::string& graphId);
    const Graph* getGraph(const std::string& graphId) const;

    /// Get the executor for a graph (valid only when active, Performance strategy).
    GraphExecutor* getExecutor(const std::string& graphId);

private:
    struct ManagedGraph {
        Graph                       graph;
        GraphState                  state = GraphState::Inactive;
        SlicingStrategy             strategy = SlicingStrategy::Performance;
        std::unique_ptr<GraphExecutor> executor;  // In-process executor (Performance mode)
        SliceResult                 sliceResult;
        std::vector<ProcessSlice>   slices;       // Child processes (multi-slice mode)
        std::vector<std::unique_ptr<rps::audio::SharedAudioRing>> bridges;  // SHM bridges
    };

    ManagedGraph& findGraph(const std::string& graphId);
    const ManagedGraph& findGraph(const std::string& graphId) const;

    mutable std::mutex m_mutex;
    std::unordered_map<std::string, ManagedGraph> m_graphs;
    uint32_t m_nextGraphId = 0;
    std::string m_hostBinaryPath;
};

} // namespace rps::coordinator
