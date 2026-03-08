#include <rps/coordinator/GraphExecutor.hpp>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <stdexcept>

namespace rps::coordinator {

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

GraphExecutor::~GraphExecutor() {
    shutdownWorkers();
}

void GraphExecutor::shutdownWorkers() {
    if (m_workers.empty()) return;

    {
        std::lock_guard lock(m_poolMutex);
        m_stopWorkers = true;
    }
    m_startCV.notify_all();

    for (auto& w : m_workers) {
        if (w.joinable()) w.join();
    }
    m_workers.clear();
}

// ---------------------------------------------------------------------------
// Worker thread — generation-counter pattern
// ---------------------------------------------------------------------------

void GraphExecutor::workerLoop() {
    uint64_t localGen = 0;

    while (true) {
        {
            std::unique_lock lock(m_poolMutex);
            m_startCV.wait(lock, [&] {
                return m_stopWorkers || m_generation > localGen;
            });
            if (m_stopWorkers) return;
            localGen = m_generation;
        }

        while (true) {
            uint32_t idx = m_nextTask.fetch_add(1, std::memory_order_relaxed);
            if (idx >= m_currentWaveSize) break;

            processNode((*m_currentWavefront)[idx]);

            uint32_t done = m_completedTasks.fetch_add(1, std::memory_order_acq_rel) + 1;
            if (done == m_currentWaveSize) {
                m_doneCV.notify_one();
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Prepare — builds all lookup tables, zero allocations afterward
// ---------------------------------------------------------------------------

void GraphExecutor::prepare(const Graph& graph, PluginProcessCallback pluginCallback) {
    shutdownWorkers();

    m_graph = &graph;
    m_pluginCallback = std::move(pluginCallback);
    m_portBuffers.clear();
    m_inputBufferLookup.clear();
    m_nodeOutputBuffer.clear();

    m_order = graph.topologicalSort();
    if (m_order.empty() && graph.nodeCount() > 0) {
        throw std::runtime_error("Cannot prepare executor: graph has a cycle");
    }

    m_wavefronts = graph.computeWavefronts();

    uint32_t blockSize = graph.config().blockSize;

    // Allocate port buffers (all string formatting happens here, once)
    for (const auto& [id, node] : graph.nodes()) {
        for (const auto& port : node.outputPorts) {
            auto key = std::format("{}:{}", id, port.index);
            uint32_t channels = port.layout.effectiveChannelCount();
            if (channels == 0) channels = 2;
            m_portBuffers.emplace(key, AudioBuffer(channels, blockSize));
        }
    }

    // Build direct pointer lookups (resolves all keys to pointers once)

    // 1. Per-node output buffer pointer (port 0)
    for (const auto& [id, node] : graph.nodes()) {
        if (!node.outputPorts.empty()) {
            auto key = std::format("{}:{}", id, 0);
            auto it = m_portBuffers.find(key);
            if (it != m_portBuffers.end()) {
                m_nodeOutputBuffer[id] = &it->second;
            }
        }
    }

    // 2. Edge lookup: destNodeId → destPort → AudioBuffer* of upstream output
    for (const auto& edge : graph.edges()) {
        auto outputKey = std::format("{}:{}", edge.sourceNodeId, edge.sourcePort);
        auto bufIt = m_portBuffers.find(outputKey);
        if (bufIt != m_portBuffers.end()) {
            m_inputBufferLookup[edge.destNodeId][edge.destPort] = &bufIt->second;
        }
    }

    // Decide: parallel or serial?
    bool hasParallelWork = false;
    for (const auto& wave : m_wavefronts) {
        if (wave.size() > 1) {
            hasParallelWork = true;
            break;
        }
    }
    m_useParallel = m_parallelEnabled && hasParallelWork;

    if (m_useParallel) {
        uint32_t threadCount = m_requestedThreadCount;
        if (threadCount == 0) {
            uint32_t hw = std::thread::hardware_concurrency();
            threadCount = std::max(2u, std::min(hw / 2, 8u));
        }

        spdlog::debug("GraphExecutor: parallel mode, {} worker threads", threadCount);
        m_stopWorkers = false;
        m_generation = 0;
        m_currentWavefront = nullptr;
        m_currentWaveSize = 0;
        m_workers.reserve(threadCount);
        for (uint32_t i = 0; i < threadCount; ++i) {
            m_workers.emplace_back(&GraphExecutor::workerLoop, this);
        }
    } else {
        spdlog::debug("GraphExecutor: serial mode ({} wavefronts, all single-node)",
                      m_wavefronts.size());
    }

    m_prepared = true;
}

// ---------------------------------------------------------------------------
// Process block
// ---------------------------------------------------------------------------

void GraphExecutor::processBlock(
    const std::unordered_map<std::string, AudioBuffer>& inputBuffers,
    std::unordered_map<std::string, AudioBuffer>& outputBuffers) {

    if (!m_prepared) {
        throw std::runtime_error("GraphExecutor::processBlock() called without prepare()");
    }

    for (auto& [_, buf] : m_portBuffers) {
        buf.clear();
    }

    m_currentInputs = &inputBuffers;
    m_currentOutputs = &outputBuffers;

    if (m_useParallel) {
        for (const auto& wave : m_wavefronts) {
            if (wave.size() == 1) {
                processNode(wave[0]);
            } else {
                uint32_t waveSize = static_cast<uint32_t>(wave.size());

                m_nextTask.store(0, std::memory_order_relaxed);
                m_completedTasks.store(0, std::memory_order_relaxed);

                {
                    std::lock_guard lock(m_poolMutex);
                    m_currentWavefront = &wave;
                    m_currentWaveSize = waveSize;
                    ++m_generation;
                }
                m_startCV.notify_all();

                while (true) {
                    uint32_t idx = m_nextTask.fetch_add(1, std::memory_order_relaxed);
                    if (idx >= waveSize) break;
                    processNode(wave[idx]);
                    uint32_t done = m_completedTasks.fetch_add(1, std::memory_order_acq_rel) + 1;
                    if (done == waveSize) {
                        m_doneCV.notify_one();
                    }
                }

                {
                    std::unique_lock lock(m_poolMutex);
                    m_doneCV.wait(lock, [&] {
                        return m_completedTasks.load(std::memory_order_acquire) >= waveSize;
                    });
                }
            }
        }
    } else {
        for (const auto& nodeId : m_order) {
            processNode(nodeId);
        }
    }

    m_currentInputs = nullptr;
    m_currentOutputs = nullptr;
}

// ---------------------------------------------------------------------------
// Single-node dispatch (thread-safe per wavefront guarantee)
// ---------------------------------------------------------------------------

void GraphExecutor::processNode(const std::string& nodeId) {
    const auto* node = m_graph->findNode(nodeId);
    if (!node) return;

    switch (node->type) {
        case NodeType::Input:
        case NodeType::SidechainInput:
        case NodeType::Receive:
            processInputNode(*node, *m_currentInputs);
            break;

        case NodeType::Output:
        case NodeType::Send:
            processOutputNode(*node, *m_currentOutputs);
            break;

        case NodeType::Gain:
            processGainNode(*node);
            break;

        case NodeType::Mixer:
            processMixerNode(*node);
            break;

        case NodeType::Plugin:
            processPluginNode(*node);
            break;

        case NodeType::ChannelRouter:
            processChannelRouterNode(*node);
            break;

        case NodeType::Downmix:
            processDownmixNode(*node);
            break;
    }
}

// ---------------------------------------------------------------------------
// O(1) input buffer lookup (pre-built pointer table, zero allocations)
// ---------------------------------------------------------------------------

AudioBuffer* GraphExecutor::getInputBuffer(const std::string& nodeId, uint32_t portIndex) {
    // Hot path: two hash lookups, zero string allocations
    auto nodeIt = m_inputBufferLookup.find(nodeId);
    if (nodeIt == m_inputBufferLookup.end()) return nullptr;
    auto portIt = nodeIt->second.find(portIndex);
    return portIt != nodeIt->second.end() ? portIt->second : nullptr;
}

// ---------------------------------------------------------------------------
// Node implementations — zero allocations in hot path
// ---------------------------------------------------------------------------

void GraphExecutor::processInputNode(
    const GraphNode& node,
    const std::unordered_map<std::string, AudioBuffer>& inputBuffers) {

    auto it = inputBuffers.find(node.id);
    if (it == inputBuffers.end()) return;

    auto outIt = m_nodeOutputBuffer.find(node.id);
    if (outIt == m_nodeOutputBuffer.end()) return;

    outIt->second->copyFrom(it->second);
}

void GraphExecutor::processOutputNode(
    const GraphNode& node,
    std::unordered_map<std::string, AudioBuffer>& outputBuffers) {

    auto* input = getInputBuffer(node.id, 0);
    if (!input) return;

    auto it = outputBuffers.find(node.id);
    if (it == outputBuffers.end()) {
        throw std::runtime_error(std::format(
            "processOutputNode: output buffer for '{}' was not pre-allocated. "
            "Callers must pre-allocate all output buffers before processBlock().",
            node.id));
    }
    it->second.copyFrom(*input);
}

void GraphExecutor::processGainNode(const GraphNode& node) {
    auto* input = getInputBuffer(node.id, 0);
    if (!input) return;

    auto outIt = m_nodeOutputBuffer.find(node.id);
    if (outIt == m_nodeOutputBuffer.end()) return;
    auto* outBuf = outIt->second;

    if (node.gainConfig && node.gainConfig->bypass) {
        outBuf->copyFrom(*input);
        return;
    }

    if (node.gainConfig && node.gainConfig->mute) {
        outBuf->clear();
        return;
    }

    outBuf->copyFrom(*input);
    float gain = node.gainConfig ? node.gainConfig->gain : 1.0f;
    outBuf->applyGain(gain);
}

void GraphExecutor::processMixerNode(const GraphNode& node) {
    auto outIt = m_nodeOutputBuffer.find(node.id);
    if (outIt == m_nodeOutputBuffer.end()) return;
    auto* outBuf = outIt->second;

    outBuf->clear();

    for (const auto& port : node.inputPorts) {
        auto* input = getInputBuffer(node.id, port.index);
        if (!input) continue;

        float gain = 1.0f;
        if (node.mixerConfig && port.index < node.mixerConfig->inputGains.size()) {
            gain = node.mixerConfig->inputGains[port.index];
        }
        outBuf->mixIn(*input, gain);
    }
}

void GraphExecutor::processPluginNode(const GraphNode& node) {
    auto* input = getInputBuffer(node.id, 0);
    if (!input) return;

    auto outIt = m_nodeOutputBuffer.find(node.id);
    if (outIt == m_nodeOutputBuffer.end()) return;
    auto* outBuf = outIt->second;

    if (m_pluginCallback) {
        bool ok = m_pluginCallback(node.id, *input, *outBuf);
        if (!ok) {
            outBuf->copyFrom(*input);
        }
    } else {
        outBuf->copyFrom(*input);
    }
}

void GraphExecutor::processChannelRouterNode(const GraphNode& node) {
    auto* input = getInputBuffer(node.id, 0);
    if (!input) return;

    auto outIt = m_nodeOutputBuffer.find(node.id);
    if (outIt == m_nodeOutputBuffer.end()) return;
    auto* outBuf = outIt->second;

    outBuf->clear();

    if (!node.channelRouterConfig) {
        outBuf->copyFrom(*input);
        return;
    }

    const auto& routing = node.channelRouterConfig->routing;
    uint32_t outChannels = outBuf->numChannels();
    uint32_t blockSize = outBuf->blockSize();

    for (uint32_t outCh = 0; outCh < outChannels && outCh < routing.size(); ++outCh) {
        uint32_t srcCh = routing[outCh];
        if (srcCh < input->numChannels()) {
            const float* src = input->channel(srcCh);
            float* dst = outBuf->channel(outCh);
            std::copy(src, src + blockSize, dst);
        }
    }
}

void GraphExecutor::processDownmixNode(const GraphNode& node) {
    auto* input = getInputBuffer(node.id, 0);
    if (!input) return;

    auto outIt = m_nodeOutputBuffer.find(node.id);
    if (outIt == m_nodeOutputBuffer.end()) return;
    auto* outBuf = outIt->second;

    outBuf->clear();

    uint32_t inCh = input->numChannels();
    uint32_t outCh = outBuf->numChannels();
    uint32_t blockSize = input->blockSize();

    if (outCh == 2 && inCh > 2) {
        float scale = 1.0f / static_cast<float>(std::max(1u, (inCh + 1) / 2));
        float* outL = outBuf->channel(0);
        float* outR = outBuf->channel(1);

        for (uint32_t ch = 0; ch < inCh; ++ch) {
            const float* src = input->channel(ch);
            float* dst = (ch % 2 == 0) ? outL : outR;
            for (uint32_t s = 0; s < blockSize; ++s) {
                double acc = static_cast<double>(dst[s])
                           + static_cast<double>(src[s]) * static_cast<double>(scale);
                dst[s] = static_cast<float>(acc);
            }
        }
    } else {
        uint32_t copy = std::min(inCh, outCh);
        for (uint32_t ch = 0; ch < copy; ++ch) {
            std::copy(input->channel(ch), input->channel(ch) + blockSize,
                      outBuf->channel(ch));
        }
    }
}

} // namespace rps::coordinator
