#include <rps/coordinator/Graph.hpp>
#include <rps/coordinator/ChannelFormat.hpp>

#include <algorithm>
#include <format>
#include <queue>
#include <stdexcept>
#include <unordered_set>

namespace rps::coordinator {

Graph::Graph(const Config& config) : m_config(config) {}

// ---------------------------------------------------------------------------
// Node management
// ---------------------------------------------------------------------------

void Graph::addNode(GraphNode node) {
    if (m_nodes.contains(node.id)) {
        throw std::invalid_argument(
            std::format("Node '{}' already exists in graph", node.id));
    }
    auto id = node.id;
    m_nodes.emplace(std::move(id), std::move(node));
}

void Graph::removeNode(const std::string& id) {
    if (!m_nodes.contains(id)) {
        throw std::invalid_argument(
            std::format("Node '{}' not found in graph", id));
    }
    // Remove all edges connected to this node
    std::erase_if(m_edges, [&](const Edge& e) {
        return e.sourceNodeId == id || e.destNodeId == id;
    });
    m_nodes.erase(id);
}

const GraphNode* Graph::findNode(const std::string& id) const {
    auto it = m_nodes.find(id);
    return it != m_nodes.end() ? &it->second : nullptr;
}

GraphNode* Graph::findNodeMut(const std::string& id) {
    auto it = m_nodes.find(id);
    return it != m_nodes.end() ? &it->second : nullptr;
}

std::vector<std::string> Graph::nodeIds() const {
    std::vector<std::string> ids;
    ids.reserve(m_nodes.size());
    for (const auto& [id, _] : m_nodes) {
        ids.push_back(id);
    }
    return ids;
}

// ---------------------------------------------------------------------------
// Edge management
// ---------------------------------------------------------------------------

std::string Graph::addEdge(const std::string& sourceNodeId, uint32_t sourcePort,
                            const std::string& destNodeId, uint32_t destPort) {
    auto* src = findNode(sourceNodeId);
    if (!src) {
        throw std::invalid_argument(
            std::format("Source node '{}' not found", sourceNodeId));
    }
    auto* dst = findNode(destNodeId);
    if (!dst) {
        throw std::invalid_argument(
            std::format("Destination node '{}' not found", destNodeId));
    }

    // Validate port indices
    if (sourcePort >= src->outputPorts.size()) {
        throw std::invalid_argument(
            std::format("Source node '{}' has no output port {}", sourceNodeId, sourcePort));
    }
    if (destPort >= dst->inputPorts.size()) {
        throw std::invalid_argument(
            std::format("Destination node '{}' has no input port {}", destNodeId, destPort));
    }

    auto edgeId = std::format("e{}", m_nextEdgeId++);
    m_edges.push_back(Edge{edgeId, sourceNodeId, sourcePort, destNodeId, destPort});
    return edgeId;
}

void Graph::removeEdge(const std::string& id) {
    auto it = std::ranges::find_if(m_edges, [&](const Edge& e) {
        return e.id == id;
    });
    if (it == m_edges.end()) {
        throw std::invalid_argument(
            std::format("Edge '{}' not found", id));
    }
    m_edges.erase(it);
}

std::vector<const Edge*> Graph::edgesFrom(const std::string& nodeId) const {
    std::vector<const Edge*> result;
    for (const auto& e : m_edges) {
        if (e.sourceNodeId == nodeId) result.push_back(&e);
    }
    return result;
}

std::vector<const Edge*> Graph::edgesTo(const std::string& nodeId) const {
    std::vector<const Edge*> result;
    for (const auto& e : m_edges) {
        if (e.destNodeId == nodeId) result.push_back(&e);
    }
    return result;
}

// ---------------------------------------------------------------------------
// Graph algorithms
// ---------------------------------------------------------------------------

std::unordered_map<std::string, std::vector<std::string>> Graph::adjacencyList() const {
    std::unordered_map<std::string, std::vector<std::string>> adj;
    // Initialize all nodes (even those with no edges)
    for (const auto& [id, _] : m_nodes) {
        adj[id]; // ensure entry exists
    }
    for (const auto& e : m_edges) {
        adj[e.sourceNodeId].push_back(e.destNodeId);
    }
    return adj;
}

std::vector<std::string> Graph::topologicalSort() const {
    auto adj = adjacencyList();

    // Compute in-degrees
    std::unordered_map<std::string, int> inDegree;
    for (const auto& [id, _] : m_nodes) {
        inDegree[id] = 0;
    }
    for (const auto& e : m_edges) {
        inDegree[e.destNodeId]++;
    }

    // Kahn's algorithm
    std::queue<std::string> queue;
    for (const auto& [id, deg] : inDegree) {
        if (deg == 0) queue.push(id);
    }

    std::vector<std::string> result;
    result.reserve(m_nodes.size());

    while (!queue.empty()) {
        auto current = queue.front();
        queue.pop();
        result.push_back(current);

        for (const auto& neighbor : adj[current]) {
            if (--inDegree[neighbor] == 0) {
                queue.push(neighbor);
            }
        }
    }

    // If we didn't visit all nodes, there's a cycle
    if (result.size() != m_nodes.size()) {
        return {}; // Cycle detected
    }
    return result;
}

bool Graph::hasCycle() const {
    return topologicalSort().empty() && !m_nodes.empty();
}

ValidationResult Graph::validate() const {
    ValidationResult result;

    // 1. Check for empty graph
    if (m_nodes.empty()) {
        result.valid = false;
        result.errors.push_back({"", "", "Graph is empty"});
        return result;
    }

    // 2. Acyclic check
    auto topoOrder = topologicalSort();
    if (topoOrder.empty()) {
        result.valid = false;
        result.errors.push_back({"", "", "Graph contains a cycle"});
        return result; // Can't do further analysis with cycles
    }

    // 3. Check that at least one InputNode and one OutputNode exist
    bool hasInput = false;
    bool hasOutput = false;
    for (const auto& [_, node] : m_nodes) {
        if (node.type == NodeType::Input || node.type == NodeType::SidechainInput ||
            node.type == NodeType::Receive) {
            hasInput = true;
        }
        if (node.type == NodeType::Output || node.type == NodeType::Send) {
            hasOutput = true;
        }
    }
    if (!hasInput) {
        result.valid = false;
        result.errors.push_back({"", "", "Graph has no input nodes"});
    }
    if (!hasOutput) {
        result.valid = false;
        result.errors.push_back({"", "", "Graph has no output nodes"});
    }

    // 4. Check that all required input ports are connected
    for (const auto& [id, node] : m_nodes) {
        // Source nodes don't need input connections
        if (node.type == NodeType::Input || node.type == NodeType::SidechainInput ||
            node.type == NodeType::Receive) {
            continue;
        }

        for (const auto& port : node.inputPorts) {
            bool connected = false;
            for (const auto& e : m_edges) {
                if (e.destNodeId == id && e.destPort == port.index) {
                    connected = true;
                    break;
                }
            }
            if (!connected) {
                result.valid = false;
                result.errors.push_back(
                    {id, "", std::format("Input port {} is not connected", port.index)});
            }
        }
    }

    // 5. Channel compatibility on edges
    for (const auto& e : m_edges) {
        auto* src = findNode(e.sourceNodeId);
        auto* dst = findNode(e.destNodeId);
        if (!src || !dst) continue;

        if (e.sourcePort < src->outputPorts.size() && e.destPort < dst->inputPorts.size()) {
            const auto& srcLayout = src->outputPorts[e.sourcePort].layout;
            const auto& dstLayout = dst->inputPorts[e.destPort].layout;
            if (!areLayoutsCompatible(srcLayout, dstLayout)) {
                result.valid = false;
                result.errors.push_back(
                    {"", e.id,
                     std::format("Channel mismatch: {}→{} ({}ch → {}ch)",
                                 e.sourceNodeId, e.destNodeId,
                                 srcLayout.effectiveChannelCount(),
                                 dstLayout.effectiveChannelCount())});
            }
        }
    }

    return result;
}

void Graph::clear() {
    m_nodes.clear();
    m_edges.clear();
    m_nextEdgeId = 1;
}

} // namespace rps::coordinator
