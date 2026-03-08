#include "TestFramework.hpp"
#include <rps/coordinator/LatencyCalculator.hpp>
#include <rps/coordinator/Graph.hpp>
#include <rps/coordinator/GraphNode.hpp>

using namespace rps::coordinator;

namespace {

ChannelLayout stereoLayout() {
    return {ChannelFormat::Stereo, 2};
}

} // anonymous namespace

TEST(Latency_SimpleChain) {
    // input(0) -> plugin(256) -> output(0)  = 256 samples
    Graph g(Graph::Config{48000, 128});
    g.addNode(createInputNode("in", {stereoLayout(), ""}));
    auto plugin = createPluginNode("plugin", {"test.vst3", "vst3"}, stereoLayout());
    plugin.latencySamples = 256;
    g.addNode(std::move(plugin));
    g.addNode(createOutputNode("out", {stereoLayout(), ""}));
    g.addEdge("in", 0, "plugin", 0);
    g.addEdge("plugin", 0, "out", 0);

    auto latencies = LatencyCalculator::compute(g);
    ASSERT_EQ(latencies["out"], 256u);
}

TEST(Latency_TwoPluginsInSeries) {
    // input(0) -> pluginA(100) -> pluginB(200) -> output(0) = 300
    Graph g(Graph::Config{48000, 128});
    g.addNode(createInputNode("in", {stereoLayout(), ""}));
    auto pA = createPluginNode("pA", {"a.vst3", "vst3"}, stereoLayout());
    pA.latencySamples = 100;
    g.addNode(std::move(pA));
    auto pB = createPluginNode("pB", {"b.vst3", "vst3"}, stereoLayout());
    pB.latencySamples = 200;
    g.addNode(std::move(pB));
    g.addNode(createOutputNode("out", {stereoLayout(), ""}));
    g.addEdge("in", 0, "pA", 0);
    g.addEdge("pA", 0, "pB", 0);
    g.addEdge("pB", 0, "out", 0);

    ASSERT_EQ(LatencyCalculator::computeForOutput(g, "out"), 300u);
}

TEST(Latency_ParallelPaths_TakesLongest) {
    // input -> pluginA(100) -> mixer -> output
    // input -> pluginB(400) -> mixer -> output
    // Longest path = 400
    Graph g(Graph::Config{48000, 128});
    g.addNode(createInputNode("in", {stereoLayout(), ""}));
    auto pA = createPluginNode("pA", {"a.vst3", "vst3"}, stereoLayout());
    pA.latencySamples = 100;
    g.addNode(std::move(pA));
    auto pB = createPluginNode("pB", {"b.vst3", "vst3"}, stereoLayout());
    pB.latencySamples = 400;
    g.addNode(std::move(pB));

    MixerNodeConfig mixCfg;
    mixCfg.outputLayout = stereoLayout();
    mixCfg.numInputs = 2;
    g.addNode(createMixerNode("mix", mixCfg));
    g.addNode(createOutputNode("out", {stereoLayout(), ""}));

    g.addEdge("in", 0, "pA", 0);
    g.addEdge("in", 0, "pB", 0);
    g.addEdge("pA", 0, "mix", 0);
    g.addEdge("pB", 0, "mix", 1);
    g.addEdge("mix", 0, "out", 0);

    ASSERT_EQ(LatencyCalculator::computeForOutput(g, "out"), 400u);
}

TEST(Latency_ZeroLatencyPassthrough) {
    // input -> gain(0) -> output  = 0
    Graph g(Graph::Config{48000, 128});
    g.addNode(createInputNode("in", {stereoLayout(), ""}));
    g.addNode(createGainNode("gain", {stereoLayout(), 0.5f, false, false}));
    g.addNode(createOutputNode("out", {stereoLayout(), ""}));
    g.addEdge("in", 0, "gain", 0);
    g.addEdge("gain", 0, "out", 0);

    ASSERT_EQ(LatencyCalculator::computeForOutput(g, "out"), 0u);
}

TEST(Latency_MultipleOutputs) {
    // input -> plugin(100) -> output_a
    // input -> plugin(100) -> gain -> output_b
    Graph g(Graph::Config{48000, 128});
    g.addNode(createInputNode("in", {stereoLayout(), ""}));
    auto p = createPluginNode("p", {"test.vst3", "vst3"}, stereoLayout());
    p.latencySamples = 100;
    g.addNode(std::move(p));
    g.addNode(createOutputNode("out_a", {stereoLayout(), ""}));
    g.addNode(createGainNode("gain", {stereoLayout(), 1.0f, false, false}));
    g.addNode(createOutputNode("out_b", {stereoLayout(), ""}));

    g.addEdge("in", 0, "p", 0);
    g.addEdge("p", 0, "out_a", 0);
    g.addEdge("p", 0, "gain", 0);
    g.addEdge("gain", 0, "out_b", 0);

    auto latencies = LatencyCalculator::compute(g);
    ASSERT_EQ(latencies["out_a"], 100u);
    ASSERT_EQ(latencies["out_b"], 100u);
}
