#include <rps/coordinator/Coordinator.hpp>
#include <rps/coordinator/GraphSerializer.hpp>
#include <rps/coordinator/LatencyCalculator.hpp>

#include <spdlog/spdlog.h>

#include <format>
#include <stdexcept>

namespace rps::coordinator {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

Coordinator::ManagedGraph& Coordinator::findGraph(const std::string& graphId) {
    auto it = m_graphs.find(graphId);
    if (it == m_graphs.end()) {
        throw std::runtime_error(std::format("Graph '{}' not found", graphId));
    }
    return it->second;
}

const Coordinator::ManagedGraph& Coordinator::findGraph(const std::string& graphId) const {
    auto it = m_graphs.find(graphId);
    if (it == m_graphs.end()) {
        throw std::runtime_error(std::format("Graph '{}' not found", graphId));
    }
    return it->second;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

Coordinator::~Coordinator() {
    // Deactivate all active graphs
    std::lock_guard lock(m_mutex);
    for (auto& [id, mg] : m_graphs) {
        if (mg.state == GraphState::Active) {
            mg.executor.reset();
            for (auto& slice : mg.slices) {
                slice.terminate();
            }
            mg.slices.clear();
            mg.state = GraphState::Inactive;
        }
    }
}

std::string Coordinator::createGraph(const Graph::Config& config) {
    std::lock_guard lock(m_mutex);
    std::string id = std::format("graph_{}", m_nextGraphId++);

    ManagedGraph mg;
    mg.graph = Graph(config);
    m_graphs.emplace(id, std::move(mg));

    spdlog::info("Coordinator: created graph '{}'", id);
    return id;
}

void Coordinator::destroyGraph(const std::string& graphId) {
    std::lock_guard lock(m_mutex);
    auto it = m_graphs.find(graphId);
    if (it == m_graphs.end()) {
        throw std::runtime_error(std::format("Graph '{}' not found", graphId));
    }

    // Deactivate first if active
    if (it->second.state == GraphState::Active) {
        it->second.executor.reset();
        for (auto& slice : it->second.slices) {
            slice.terminate();
        }
        it->second.slices.clear();
    }

    m_graphs.erase(it);
    spdlog::info("Coordinator: destroyed graph '{}'", graphId);
}

// ---------------------------------------------------------------------------
// Node management
// ---------------------------------------------------------------------------

std::string Coordinator::addNode(const std::string& graphId, GraphNode node) {
    std::lock_guard lock(m_mutex);
    auto& mg = findGraph(graphId);
    if (mg.state == GraphState::Active) {
        throw std::runtime_error("Cannot add nodes to an active graph");
    }
    std::string nodeId = node.id;
    mg.graph.addNode(std::move(node));
    return nodeId;
}

void Coordinator::removeNode(const std::string& graphId, const std::string& nodeId) {
    std::lock_guard lock(m_mutex);
    auto& mg = findGraph(graphId);
    if (mg.state == GraphState::Active) {
        throw std::runtime_error("Cannot remove nodes from an active graph");
    }
    mg.graph.removeNode(nodeId);
}

// ---------------------------------------------------------------------------
// Edge management
// ---------------------------------------------------------------------------

std::string Coordinator::connectNodes(const std::string& graphId,
                                       const std::string& sourceNodeId, uint32_t sourcePort,
                                       const std::string& destNodeId, uint32_t destPort) {
    std::lock_guard lock(m_mutex);
    auto& mg = findGraph(graphId);
    if (mg.state == GraphState::Active) {
        throw std::runtime_error("Cannot connect nodes in an active graph");
    }
    return mg.graph.addEdge(sourceNodeId, sourcePort, destNodeId, destPort);
}

void Coordinator::disconnectNodes(const std::string& graphId, const std::string& edgeId) {
    std::lock_guard lock(m_mutex);
    auto& mg = findGraph(graphId);
    if (mg.state == GraphState::Active) {
        throw std::runtime_error("Cannot disconnect nodes in an active graph");
    }
    mg.graph.removeEdge(edgeId);
}

// ---------------------------------------------------------------------------
// Validation & Activation
// ---------------------------------------------------------------------------

ValidationResult Coordinator::validateGraph(const std::string& graphId) {
    std::lock_guard lock(m_mutex);
    const auto& mg = findGraph(graphId);
    return mg.graph.validate();
}

void Coordinator::activateGraph(const std::string& graphId,
                                 SlicingStrategy strategy,
                                 PluginProcessCallback pluginCallback) {
    std::lock_guard lock(m_mutex);
    auto& mg = findGraph(graphId);

    if (mg.state == GraphState::Active) {
        throw std::runtime_error("Graph is already active");
    }

    // Validate first
    auto validation = mg.graph.validate();
    if (!validation.valid) {
        std::string errors;
        for (const auto& e : validation.errors) {
            errors += e.message + "; ";
        }
        throw std::runtime_error(std::format("Graph validation failed: {}", errors));
    }

    mg.strategy = strategy;

    // Slice the graph
    mg.sliceResult = sliceGraph(mg.graph, strategy);

    if (mg.sliceResult.isSingleSlice()) {
        // In-process mode: use GraphExecutor directly
        spdlog::info("Coordinator: activating graph '{}' in single-slice mode", graphId);
        mg.executor = std::make_unique<GraphExecutor>();
        mg.executor->prepare(mg.graph, std::move(pluginCallback));
    } else {
        // Multi-slice mode (Phase 7B: actually spawn processes)
        spdlog::info("Coordinator: graph '{}' sliced into {} subgraphs with {} bridges",
                     graphId, mg.sliceResult.slices.size(), mg.sliceResult.bridges.size());

        // For now, log the slicing result. Actual process spawning is Phase 7B.
        for (size_t i = 0; i < mg.sliceResult.slices.size(); ++i) {
            const auto& slice = mg.sliceResult.slices[i];
            spdlog::info("  Slice {}: {} nodes, {} edges",
                         i, slice.nodeCount(), slice.edges().size());
        }
        for (const auto& bridge : mg.sliceResult.bridges) {
            spdlog::info("  Bridge: slice {} → slice {} via '{}'",
                         bridge.sourceSlice, bridge.destSlice, bridge.shmName);
        }
    }

    mg.state = GraphState::Active;
    spdlog::info("Coordinator: graph '{}' activated", graphId);
}

void Coordinator::deactivateGraph(const std::string& graphId) {
    std::lock_guard lock(m_mutex);
    auto& mg = findGraph(graphId);

    if (mg.state != GraphState::Active) {
        throw std::runtime_error("Graph is not active");
    }

    // Tear down executor and slices
    mg.executor.reset();
    for (auto& slice : mg.slices) {
        slice.terminate();
    }
    mg.slices.clear();
    mg.sliceResult = {};

    mg.state = GraphState::Inactive;
    spdlog::info("Coordinator: graph '{}' deactivated", graphId);
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

GraphInfo Coordinator::getGraphInfo(const std::string& graphId) {
    std::lock_guard lock(m_mutex);
    const auto& mg = findGraph(graphId);

    GraphInfo info;
    info.graphId = graphId;
    info.state = mg.state;
    info.nodeCount = static_cast<uint32_t>(mg.graph.nodeCount());
    info.edgeCount = static_cast<uint32_t>(mg.graph.edges().size());
    info.sliceCount = static_cast<uint32_t>(mg.sliceResult.slices.size());
    info.strategy = mg.strategy;
    return info;
}

uint32_t Coordinator::getOutputLatency(const std::string& graphId,
                                        const std::string& outputNodeId) {
    std::lock_guard lock(m_mutex);
    const auto& mg = findGraph(graphId);
    auto latencies = LatencyCalculator::compute(mg.graph);
    auto it = latencies.find(outputNodeId);
    if (it == latencies.end()) return 0;
    return it->second;
}

// ---------------------------------------------------------------------------
// Serialization
// ---------------------------------------------------------------------------

std::string Coordinator::serializeGraph(const std::string& graphId) {
    std::lock_guard lock(m_mutex);
    const auto& mg = findGraph(graphId);
    return GraphSerializer::toJson(mg.graph);
}

std::string Coordinator::deserializeGraph(const std::string& jsonData) {
    Graph graph = GraphSerializer::fromJson(jsonData);

    std::lock_guard lock(m_mutex);
    std::string id = std::format("graph_{}", m_nextGraphId++);

    ManagedGraph mg;
    mg.graph = std::move(graph);
    m_graphs.emplace(id, std::move(mg));

    spdlog::info("Coordinator: deserialized graph '{}' from JSON", id);
    return id;
}

// ---------------------------------------------------------------------------
// Direct access
// ---------------------------------------------------------------------------

Graph* Coordinator::getGraph(const std::string& graphId) {
    std::lock_guard lock(m_mutex);
    auto it = m_graphs.find(graphId);
    return it != m_graphs.end() ? &it->second.graph : nullptr;
}

const Graph* Coordinator::getGraph(const std::string& graphId) const {
    std::lock_guard lock(m_mutex);
    auto it = m_graphs.find(graphId);
    return it != m_graphs.end() ? &it->second.graph : nullptr;
}

GraphExecutor* Coordinator::getExecutor(const std::string& graphId) {
    std::lock_guard lock(m_mutex);
    auto it = m_graphs.find(graphId);
    if (it == m_graphs.end() || !it->second.executor) return nullptr;
    return it->second.executor.get();
}

} // namespace rps::coordinator
