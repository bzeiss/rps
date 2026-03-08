#pragma once

#include <rps/coordinator/AudioBuffer.hpp>
#include <rps/coordinator/Graph.hpp>
#include <rps/core/CpuTopology.hpp>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace rps::coordinator {

// ---------------------------------------------------------------------------
// INodeProcessor — interface for nodes that process audio
// ---------------------------------------------------------------------------

/// External processing callback for PluginNodes.
/// graphExecutor provides this to let the caller inject real plugin processing.
/// THREAD SAFETY: This callback may be invoked from multiple threads simultaneously
/// with different nodeIds and different buffers. Implementations must ensure no
/// shared mutable state between invocations for different nodes.
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
///
/// Supports two execution modes:
/// - **Serial** (default for simple graphs): Processes nodes in topological order
///   on the calling thread. Zero overhead.
/// - **Parallel** (automatic for graphs with independent branches): Processes
///   wavefronts in parallel using a thread pool with generation-counter sync.
///   Nodes in the same wavefront have no data dependencies.
class GraphExecutor {
public:
    GraphExecutor() = default;
    ~GraphExecutor();

    // Non-copyable, non-movable (owns threads)
    GraphExecutor(const GraphExecutor&) = delete;
    GraphExecutor& operator=(const GraphExecutor&) = delete;

    /// Prepare the executor for the given graph.
    /// Allocates all internal buffers, computes wavefronts, and optionally
    /// creates a thread pool for parallel execution.
    /// The graph must be valid (pass Graph::validate() first).
    /// @param pluginCallback  Called for each PluginNode during processing.
    void prepare(const Graph& graph, PluginProcessCallback pluginCallback = nullptr);

    /// Process one block of audio through the graph.
    /// @param inputBuffers   Map of InputNode id → AudioBuffer with incoming audio.
    /// @param outputBuffers  Map of OutputNode id → AudioBuffer (must be pre-allocated
    ///                       for all OutputNode ids). Throws if a required output is missing.
    void processBlock(
        const std::unordered_map<std::string, AudioBuffer>& inputBuffers,
        std::unordered_map<std::string, AudioBuffer>& outputBuffers);

    /// Check if the executor is prepared.
    bool isPrepared() const { return m_prepared; }

    /// Get the processing order (topological sort result).
    const std::vector<std::string>& processingOrder() const { return m_order; }

    /// Get wavefronts (nodes grouped by depth for parallel execution).
    const std::vector<std::vector<std::string>>& wavefronts() const { return m_wavefronts; }

    /// Enable or disable parallel execution (for testing/benchmarking).
    /// Must be called before prepare(). If called after, takes effect on next prepare().
    void setParallelEnabled(bool enabled) { m_parallelEnabled = enabled; }

    /// Returns true if parallel execution is active.
    bool isParallel() const { return m_useParallel; }

    /// Set the number of worker threads. 0 = auto-detect.
    /// Must be called before prepare().
    void setThreadCount(uint32_t count) { m_requestedThreadCount = count; }

private:
    bool m_prepared = false;
    const Graph* m_graph = nullptr;
    PluginProcessCallback m_pluginCallback;

    /// Topological processing order (flat)
    std::vector<std::string> m_order;

    /// Wavefronts: nodes grouped by topological depth
    std::vector<std::vector<std::string>> m_wavefronts;

    /// Internal buffers: one per output port, keyed by pre-computed "nodeId:portIndex"
    std::unordered_map<std::string, AudioBuffer> m_portBuffers;

    /// Pre-built direct pointer lookup: nodeId → portIndex → AudioBuffer*
    /// Points into m_portBuffers. Built once in prepare(), gives O(1) getInputBuffer()
    /// with zero string allocations.
    std::unordered_map<std::string, std::unordered_map<uint32_t, AudioBuffer*>> m_inputBufferLookup;

    /// Per-node pre-computed output buffer pointer (nodeId → AudioBuffer* for port 0).
    /// Points into m_portBuffers. Eliminates all string formatting from the hot path.
    std::unordered_map<std::string, AudioBuffer*> m_nodeOutputBuffer;

    /// Scratch buffer for mixing
    AudioBuffer m_scratchBuffer;

    /// Parallel execution configuration
    bool m_parallelEnabled = true;     // User setting
    bool m_useParallel = false;        // Actual mode (auto-detected)
    uint32_t m_requestedThreadCount = 0; // 0 = auto

    // CPU topology (discovered once when parallel mode starts)
    rps::core::CpuTopologyInfo m_topology;

    // -- Thread pool --
    std::vector<std::thread> m_workers;
    std::mutex m_poolMutex;
    std::condition_variable m_startCV;   // workers wait on this
    std::condition_variable m_doneCV;    // main thread waits on this

    // Per-wavefront dispatch state
    const std::vector<std::string>* m_currentWavefront = nullptr;
    uint32_t m_currentWaveSize = 0;
    std::atomic<uint32_t> m_nextTask{0};
    std::atomic<uint32_t> m_completedTasks{0};
    uint64_t m_generation = 0;          // incremented each new wavefront
    bool m_stopWorkers = false;

    // Pointers to current I/O buffers (set per-processBlock call)
    const std::unordered_map<std::string, AudioBuffer>* m_currentInputs = nullptr;
    std::unordered_map<std::string, AudioBuffer>* m_currentOutputs = nullptr;

    void shutdownWorkers();
    void workerLoop();

    // Process a single node (called from any thread)
    void processNode(const std::string& nodeId);

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

    /// O(1) lookup: get the upstream output buffer for a node's input port.
    AudioBuffer* getInputBuffer(const std::string& nodeId, uint32_t portIndex);
};

} // namespace rps::coordinator
