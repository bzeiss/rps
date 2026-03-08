#pragma once

#include <rps/coordinator/GraphNode.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace rps::coordinator {

// ---------------------------------------------------------------------------
// Edge — a connection between two ports in the graph
// ---------------------------------------------------------------------------

struct Edge {
    std::string  id;
    std::string  sourceNodeId;
    uint32_t     sourcePort = 0;
    std::string  destNodeId;
    uint32_t     destPort = 0;
};

// ---------------------------------------------------------------------------
// Validation result
// ---------------------------------------------------------------------------

struct ValidationError {
    std::string nodeId;      // Empty for graph-level errors
    std::string edgeId;      // Empty for node-level errors
    std::string message;
};

struct ValidationResult {
    bool valid = true;
    std::vector<ValidationError> errors;
};

// ---------------------------------------------------------------------------
// Graph — a directed acyclic graph of audio processing nodes
// ---------------------------------------------------------------------------

class Graph {
public:
    /// Graph-level audio configuration.
    struct Config {
        uint32_t sampleRate = 48000;
        uint32_t blockSize  = 128;
    };

    Graph() = default;
    explicit Graph(const Config& config);

    // -- Configuration --
    const Config& config() const { return m_config; }
    void setConfig(const Config& config) { m_config = config; }

    // -- Node management --

    /// Add a pre-configured node to the graph. Throws if id already exists.
    void addNode(GraphNode node);

    /// Remove a node and all its connected edges. Throws if id not found.
    void removeNode(const std::string& id);

    /// Get a node by id. Returns nullptr if not found.
    const GraphNode* findNode(const std::string& id) const;
    GraphNode* findNodeMut(const std::string& id);

    /// Get all node ids.
    std::vector<std::string> nodeIds() const;

    /// Get all nodes.
    const std::unordered_map<std::string, GraphNode>& nodes() const { return m_nodes; }

    /// Number of nodes.
    size_t nodeCount() const { return m_nodes.size(); }

    // -- Edge management --

    /// Connect sourceNode:sourcePort → destNode:destPort.
    /// Returns the generated edge id. Throws on invalid node/port references.
    std::string addEdge(const std::string& sourceNodeId, uint32_t sourcePort,
                        const std::string& destNodeId, uint32_t destPort);

    /// Remove an edge by id. Throws if not found.
    void removeEdge(const std::string& id);

    /// Get all edges.
    const std::vector<Edge>& edges() const { return m_edges; }

    /// Find all edges originating from a node (fan-out).
    std::vector<const Edge*> edgesFrom(const std::string& nodeId) const;

    /// Find all edges terminating at a node (fan-in).
    std::vector<const Edge*> edgesTo(const std::string& nodeId) const;

    // -- Graph algorithms --

    /// Compute a topological ordering. Returns empty if the graph has a cycle.
    std::vector<std::string> topologicalSort() const;

    /// Check if the graph contains a cycle.
    bool hasCycle() const;

    /// Full validation: acyclic, connected, channel compatibility, no dangling ports.
    ValidationResult validate() const;

    /// Clear all nodes and edges.
    void clear();

private:
    Config m_config;
    std::unordered_map<std::string, GraphNode> m_nodes;
    std::vector<Edge> m_edges;
    uint32_t m_nextEdgeId = 1;

    /// Build adjacency list for algorithms.
    std::unordered_map<std::string, std::vector<std::string>> adjacencyList() const;
};

} // namespace rps::coordinator
