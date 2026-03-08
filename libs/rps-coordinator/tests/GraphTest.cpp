#include "TestFramework.hpp"
#include <rps/coordinator/Graph.hpp>
#include <rps/coordinator/GraphNode.hpp>
#include <rps/coordinator/ChannelFormat.hpp>

using namespace rps::coordinator;

namespace {

ChannelLayout stereoLayout() {
    return {ChannelFormat::Stereo, 2};
}

Graph buildSimpleChain() {
    // input -> gain -> output
    Graph g(Graph::Config{48000, 128});
    g.addNode(createInputNode("in", {stereoLayout(), "shm_in"}));
    g.addNode(createGainNode("gain", {stereoLayout(), 0.5f, false, false}));
    g.addNode(createOutputNode("out", {stereoLayout(), "shm_out"}));
    g.addEdge("in", 0, "gain", 0);
    g.addEdge("gain", 0, "out", 0);
    return g;
}

} // anonymous namespace

TEST(Graph_AddAndFindNode) {
    Graph g;
    g.addNode(createInputNode("in1", {stereoLayout(), ""}));
    ASSERT_TRUE(g.findNode("in1") != nullptr);
    ASSERT_TRUE(g.findNode("nonexistent") == nullptr);
    ASSERT_EQ(g.nodeCount(), 1u);
}

TEST(Graph_DuplicateNodeThrows) {
    Graph g;
    g.addNode(createInputNode("in1", {stereoLayout(), ""}));
    ASSERT_THROWS(g.addNode(createInputNode("in1", {stereoLayout(), ""})));
}

TEST(Graph_RemoveNode) {
    Graph g;
    g.addNode(createInputNode("in1", {stereoLayout(), ""}));
    g.addNode(createOutputNode("out1", {stereoLayout(), ""}));
    g.addEdge("in1", 0, "out1", 0);
    ASSERT_EQ(g.edges().size(), 1u);
    g.removeNode("in1");
    ASSERT_EQ(g.nodeCount(), 1u);
    ASSERT_EQ(g.edges().size(), 0u); // Edge should be removed too
}

TEST(Graph_AddEdge_InvalidNodeThrows) {
    Graph g;
    g.addNode(createInputNode("in1", {stereoLayout(), ""}));
    ASSERT_THROWS(g.addEdge("in1", 0, "nonexistent", 0));
}

TEST(Graph_AddEdge_InvalidPortThrows) {
    Graph g;
    g.addNode(createInputNode("in1", {stereoLayout(), ""}));
    g.addNode(createOutputNode("out1", {stereoLayout(), ""}));
    ASSERT_THROWS(g.addEdge("in1", 99, "out1", 0)); // port 99 doesn't exist
}

TEST(Graph_TopologicalSort_SimpleChain) {
    auto g = buildSimpleChain();
    auto order = g.topologicalSort();
    ASSERT_EQ(order.size(), 3u);

    // "in" must come before "gain", "gain" before "out"
    size_t posIn = 999, posGain = 999, posOut = 999;
    for (size_t i = 0; i < order.size(); ++i) {
        if (order[i] == "in") posIn = i;
        if (order[i] == "gain") posGain = i;
        if (order[i] == "out") posOut = i;
    }
    ASSERT_TRUE(posIn < posGain);
    ASSERT_TRUE(posGain < posOut);
}

TEST(Graph_TopologicalSort_CycleReturnsEmpty) {
    Graph g;
    g.addNode(createGainNode("a", {stereoLayout(), 1.0f, false, false}));
    g.addNode(createGainNode("b", {stereoLayout(), 1.0f, false, false}));
    g.addEdge("a", 0, "b", 0);
    g.addEdge("b", 0, "a", 0);
    auto order = g.topologicalSort();
    ASSERT_TRUE(order.empty());
}

TEST(Graph_HasCycle) {
    Graph g;
    g.addNode(createGainNode("a", {stereoLayout(), 1.0f, false, false}));
    g.addNode(createGainNode("b", {stereoLayout(), 1.0f, false, false}));
    g.addEdge("a", 0, "b", 0);
    g.addEdge("b", 0, "a", 0);
    ASSERT_TRUE(g.hasCycle());
}

TEST(Graph_NoCycle) {
    auto g = buildSimpleChain();
    ASSERT_FALSE(g.hasCycle());
}

TEST(Graph_Validate_ValidChain) {
    auto g = buildSimpleChain();
    auto result = g.validate();
    ASSERT_TRUE(result.valid);
    ASSERT_TRUE(result.errors.empty());
}

TEST(Graph_Validate_NoInputNode) {
    Graph g;
    g.addNode(createOutputNode("out", {stereoLayout(), ""}));
    auto result = g.validate();
    ASSERT_FALSE(result.valid);
}

TEST(Graph_Validate_NoOutputNode) {
    Graph g;
    g.addNode(createInputNode("in", {stereoLayout(), ""}));
    auto result = g.validate();
    ASSERT_FALSE(result.valid);
}

TEST(Graph_Validate_DisconnectedPort) {
    Graph g;
    g.addNode(createInputNode("in", {stereoLayout(), ""}));
    g.addNode(createGainNode("gain", {stereoLayout(), 1.0f, false, false}));
    g.addNode(createOutputNode("out", {stereoLayout(), ""}));
    g.addEdge("in", 0, "gain", 0);
    // gain -> out is NOT connected
    auto result = g.validate();
    ASSERT_FALSE(result.valid);
}

TEST(Graph_Validate_ChannelMismatch) {
    Graph g;
    ChannelLayout mono{ChannelFormat::Mono, 1};
    g.addNode(createInputNode("in", {stereoLayout(), ""}));
    g.addNode(createOutputNode("out", {mono, ""}));
    g.addEdge("in", 0, "out", 0);
    auto result = g.validate();
    ASSERT_FALSE(result.valid);
}

TEST(Graph_FanOut) {
    // input -> gain_a and input -> gain_b (fan-out from input)
    Graph g(Graph::Config{48000, 128});
    g.addNode(createInputNode("in", {stereoLayout(), ""}));
    g.addNode(createGainNode("a", {stereoLayout(), 0.5f, false, false}));
    g.addNode(createGainNode("b", {stereoLayout(), 0.8f, false, false}));
    MixerNodeConfig mixCfg;
    mixCfg.outputLayout = stereoLayout();
    mixCfg.numInputs = 2;
    g.addNode(createMixerNode("mix", mixCfg));
    g.addNode(createOutputNode("out", {stereoLayout(), ""}));
    g.addEdge("in", 0, "a", 0);
    g.addEdge("in", 0, "b", 0);
    g.addEdge("a", 0, "mix", 0);
    g.addEdge("b", 0, "mix", 1);
    g.addEdge("mix", 0, "out", 0);

    auto result = g.validate();
    ASSERT_TRUE(result.valid);

    auto order = g.topologicalSort();
    ASSERT_EQ(order.size(), 5u);
}

TEST(Graph_EdgeQueries) {
    auto g = buildSimpleChain();
    auto from = g.edgesFrom("in");
    ASSERT_EQ(from.size(), 1u);
    ASSERT_EQ(from[0]->destNodeId, "gain");

    auto to = g.edgesTo("out");
    ASSERT_EQ(to.size(), 1u);
    ASSERT_EQ(to[0]->sourceNodeId, "gain");
}

TEST(Graph_Clear) {
    auto g = buildSimpleChain();
    ASSERT_TRUE(g.nodeCount() > 0);
    g.clear();
    ASSERT_EQ(g.nodeCount(), 0u);
    ASSERT_EQ(g.edges().size(), 0u);
}

// ---------------------------------------------------------------------------
// Phase 5: Wavefront tests
// ---------------------------------------------------------------------------

TEST(Graph_Wavefronts_SimpleChain) {
    // input -> gain -> output: 3 wavefronts, each with 1 node
    auto g = buildSimpleChain();
    auto waves = g.computeWavefronts();
    ASSERT_EQ(waves.size(), 3u);
    ASSERT_EQ(waves[0].size(), 1u); // in
    ASSERT_EQ(waves[1].size(), 1u); // gain
    ASSERT_EQ(waves[2].size(), 1u); // out
}

TEST(Graph_Wavefronts_Diamond) {
    // in -> A -> mixer -> out
    // in -> B -> mixer -> out
    // Wavefronts: [in], [A, B], [mixer], [out]
    Graph g(Graph::Config{48000, 128});
    g.addNode(createInputNode("in", {stereoLayout(), ""}));
    g.addNode(createGainNode("A", {stereoLayout(), 0.5f, false, false}));
    g.addNode(createGainNode("B", {stereoLayout(), 0.8f, false, false}));
    MixerNodeConfig mixCfg;
    mixCfg.outputLayout = stereoLayout();
    mixCfg.numInputs = 2;
    g.addNode(createMixerNode("mix", mixCfg));
    g.addNode(createOutputNode("out", {stereoLayout(), ""}));
    g.addEdge("in", 0, "A", 0);
    g.addEdge("in", 0, "B", 0);
    g.addEdge("A", 0, "mix", 0);
    g.addEdge("B", 0, "mix", 1);
    g.addEdge("mix", 0, "out", 0);

    auto waves = g.computeWavefronts();
    ASSERT_EQ(waves.size(), 4u);
    ASSERT_EQ(waves[0].size(), 1u); // in
    ASSERT_EQ(waves[1].size(), 2u); // A and B (parallel!)
    ASSERT_EQ(waves[2].size(), 1u); // mix
    ASSERT_EQ(waves[3].size(), 1u); // out
}

TEST(Graph_Wavefronts_Wide) {
    // 4 parallel input->gain chains merging at a mixer
    // Wavefronts: [in1,in2,in3,in4], [g1,g2,g3,g4], [mix], [out]
    Graph g(Graph::Config{48000, 128});
    for (int i = 1; i <= 4; ++i) {
        auto inId = "in" + std::to_string(i);
        auto gId = "g" + std::to_string(i);
        g.addNode(createInputNode(inId, {stereoLayout(), ""}));
        g.addNode(createGainNode(gId, {stereoLayout(), 1.0f, false, false}));
        g.addEdge(inId, 0, gId, 0);
    }
    MixerNodeConfig mixCfg;
    mixCfg.outputLayout = stereoLayout();
    mixCfg.numInputs = 4;
    g.addNode(createMixerNode("mix", mixCfg));
    g.addNode(createOutputNode("out", {stereoLayout(), ""}));
    for (int i = 1; i <= 4; ++i) {
        g.addEdge("g" + std::to_string(i), 0, "mix", static_cast<uint32_t>(i - 1));
    }
    g.addEdge("mix", 0, "out", 0);

    auto waves = g.computeWavefronts();
    ASSERT_EQ(waves.size(), 4u);
    ASSERT_EQ(waves[0].size(), 4u); // 4 inputs
    ASSERT_EQ(waves[1].size(), 4u); // 4 gains
    ASSERT_EQ(waves[2].size(), 1u); // mixer
    ASSERT_EQ(waves[3].size(), 1u); // output
}

TEST(Graph_Wavefronts_CycleReturnsEmpty) {
    Graph g;
    g.addNode(createGainNode("a", {stereoLayout(), 1.0f, false, false}));
    g.addNode(createGainNode("b", {stereoLayout(), 1.0f, false, false}));
    g.addEdge("a", 0, "b", 0);
    g.addEdge("b", 0, "a", 0);
    auto waves = g.computeWavefronts();
    ASSERT_TRUE(waves.empty());
}
