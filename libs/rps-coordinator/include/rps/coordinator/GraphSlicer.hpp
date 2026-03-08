#pragma once

#include <rps/coordinator/Graph.hpp>

#include <string>
#include <vector>

namespace rps::coordinator {

// ---------------------------------------------------------------------------
// Slicing strategies
// ---------------------------------------------------------------------------

enum class SlicingStrategy : uint8_t {
    /// Keep the entire graph in a single slice. No IPC overhead.
    /// Best for simple graphs or when crash isolation is not needed.
    Performance = 0,

    /// Group nodes by their sliceHint field. Nodes with the same sliceHint
    /// go into the same slice. Non-plugin nodes are assigned to the slice
    /// of their nearest upstream plugin. If no sliceHint is set, all plugins
    /// go into a single "main" slice.
    Default = 1,

    /// One slice per PluginNode. Maximum crash isolation — if any plugin
    /// crashes, only its process dies. Most IPC overhead.
    CrashIsolation = 2,
};

// ---------------------------------------------------------------------------
// Slicing result
// ---------------------------------------------------------------------------

/// Describes a shared-memory bridge between two slices.
struct ShmBridge {
    std::string sendNodeId;       ///< SendNode ID in the source slice
    std::string receiveNodeId;    ///< ReceiveNode ID in the destination slice
    std::string shmName;          ///< Unique shared memory segment name
    uint32_t    sourceSlice = 0;  ///< Index into SliceResult::slices
    uint32_t    destSlice = 0;    ///< Index into SliceResult::slices
};

struct SliceResult {
    /// The subgraphs. Each is a self-contained, independently valid Graph.
    std::vector<Graph> slices;

    /// Shared memory bridges connecting slices.
    std::vector<ShmBridge> bridges;

    /// True if no slicing was needed (single slice = the original graph).
    bool isSingleSlice() const { return slices.size() <= 1 && bridges.empty(); }
};

// ---------------------------------------------------------------------------
// Graph slicing
// ---------------------------------------------------------------------------

/// Partition a graph into subgraphs (slices) based on the given strategy.
///
/// For each edge that crosses a slice boundary, a SendNode is inserted in the
/// source slice and a ReceiveNode in the destination slice, connected by a
/// unique shared memory segment name.
///
/// Each output subgraph independently passes Graph::validate().
///
/// @param graph     The full, validated graph to partition.
/// @param strategy  How to assign nodes to slices.
/// @return          The slicing result with subgraphs and bridge descriptors.
SliceResult sliceGraph(const Graph& graph, SlicingStrategy strategy);

} // namespace rps::coordinator
