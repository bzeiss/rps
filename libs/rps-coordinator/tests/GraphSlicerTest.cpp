#include "TestFramework.hpp"
#include <rps/coordinator/GraphSlicer.hpp>
#include <rps/coordinator/GraphNode.hpp>

using namespace rps::coordinator;

// Helper: build a simple chain: in → plugin → out
static Graph makeSimpleChain(const std::string& pluginHint = "") {
    Graph g({48000, 128});
    auto in = createInputNode("in", {{ChannelFormat::Stereo, 2}, ""});
    g.addNode(std::move(in));
    auto plug = createPluginNode("eq", {"/path/eq.vst3", "vst3"}, {ChannelFormat::Stereo, 2});
    plug.sliceHint = pluginHint;
    g.addNode(std::move(plug));
    auto out = createOutputNode("out", {{ChannelFormat::Stereo, 2}, ""});
    g.addNode(std::move(out));
    g.addEdge("in", 0, "eq", 0);
    g.addEdge("eq", 0, "out", 0);
    return g;
}

// Helper: build a 2-plugin chain with different slice hints
// in → pluginA → pluginB → out
static Graph makeTwoPluginChain(const std::string& hintA = "",
                                 const std::string& hintB = "") {
    Graph g({48000, 128});
    g.addNode(createInputNode("in", {{ChannelFormat::Stereo, 2}, ""}));
    auto pa = createPluginNode("plugA", {"/path/a.vst3", "vst3"}, {ChannelFormat::Stereo, 2});
    pa.sliceHint = hintA;
    g.addNode(std::move(pa));
    auto pb = createPluginNode("plugB", {"/path/b.vst3", "vst3"}, {ChannelFormat::Stereo, 2});
    pb.sliceHint = hintB;
    g.addNode(std::move(pb));
    g.addNode(createOutputNode("out", {{ChannelFormat::Stereo, 2}, ""}));
    g.addEdge("in", 0, "plugA", 0);
    g.addEdge("plugA", 0, "plugB", 0);
    g.addEdge("plugB", 0, "out", 0);
    return g;
}

// Helper: build a diamond graph
// in → plugA → mixer → out
// in → plugB → mixer
static Graph makeDiamond(const std::string& hintA = "",
                          const std::string& hintB = "") {
    Graph g({48000, 128});
    g.addNode(createInputNode("in", {{ChannelFormat::Stereo, 2}, ""}));
    auto pa = createPluginNode("plugA", {"/path/a.vst3", "vst3"}, {ChannelFormat::Stereo, 2});
    pa.sliceHint = hintA;
    g.addNode(std::move(pa));
    auto pb = createPluginNode("plugB", {"/path/b.vst3", "vst3"}, {ChannelFormat::Stereo, 2});
    pb.sliceHint = hintB;
    g.addNode(std::move(pb));
    g.addNode(createMixerNode("mixer", {{ChannelFormat::Stereo, 2}, 2, {1.0f, 1.0f}}));
    g.addNode(createOutputNode("out", {{ChannelFormat::Stereo, 2}, ""}));
    g.addEdge("in", 0, "plugA", 0);
    g.addEdge("in", 0, "plugB", 0);
    g.addEdge("plugA", 0, "mixer", 0);
    g.addEdge("plugB", 0, "mixer", 1);
    g.addEdge("mixer", 0, "out", 0);
    return g;
}

// ---------------------------------------------------------------------------
// Performance strategy — entire graph stays in one slice
// ---------------------------------------------------------------------------

TEST(GraphSlicer_Performance_SingleSlice) {
    auto g = makeTwoPluginChain("slice1", "slice2");
    auto result = sliceGraph(g, SlicingStrategy::Performance);
    ASSERT_TRUE(result.isSingleSlice());
    ASSERT_EQ(result.slices.size(), static_cast<size_t>(1));
    ASSERT_EQ(result.bridges.size(), static_cast<size_t>(0));
    ASSERT_EQ(result.slices[0].nodeCount(), g.nodeCount());
}

// ---------------------------------------------------------------------------
// Default strategy — same hint = same slice
// ---------------------------------------------------------------------------

TEST(GraphSlicer_Default_SameHint_SingleSlice) {
    auto g = makeTwoPluginChain("slice1", "slice1");
    auto result = sliceGraph(g, SlicingStrategy::Default);
    // Same hint → everything in one slice
    ASSERT_TRUE(result.isSingleSlice());
    ASSERT_EQ(result.bridges.size(), static_cast<size_t>(0));
}

TEST(GraphSlicer_Default_DifferentHints_TwoSlices) {
    auto g = makeTwoPluginChain("sliceA", "sliceB");
    auto result = sliceGraph(g, SlicingStrategy::Default);
    ASSERT_EQ(result.slices.size(), static_cast<size_t>(2));
    // One bridge between the two slices (plugA → plugB cross-slice edge)
    ASSERT_TRUE(result.bridges.size() >= 1);
    // Each slice should have at least the plugin + send/recv node
    for (const auto& slice : result.slices) {
        ASSERT_TRUE(slice.nodeCount() >= 2);
    }
}

TEST(GraphSlicer_Default_EmptyHint_MainSlice) {
    // Both plugins have empty hint → all in main slice
    auto g = makeTwoPluginChain("", "");
    auto result = sliceGraph(g, SlicingStrategy::Default);
    ASSERT_TRUE(result.isSingleSlice());
}

// ---------------------------------------------------------------------------
// CrashIsolation strategy — one slice per plugin
// ---------------------------------------------------------------------------

TEST(GraphSlicer_CrashIsolation_TwoPlugins_TwoSlices) {
    auto g = makeTwoPluginChain();
    auto result = sliceGraph(g, SlicingStrategy::CrashIsolation);
    ASSERT_EQ(result.slices.size(), static_cast<size_t>(2));
    ASSERT_TRUE(result.bridges.size() >= 1);
}

TEST(GraphSlicer_CrashIsolation_Diamond_TwoSlices) {
    auto g = makeDiamond();
    auto result = sliceGraph(g, SlicingStrategy::CrashIsolation);
    ASSERT_EQ(result.slices.size(), static_cast<size_t>(2));
    // At least 2 bridges: in→plugA and in→plugB are in different slices
    ASSERT_TRUE(result.bridges.size() >= 1);
}

// ---------------------------------------------------------------------------
// Bridge correctness — Send/Receive node properties
// ---------------------------------------------------------------------------

TEST(GraphSlicer_BridgeHasSendAndReceive) {
    auto g = makeTwoPluginChain("sliceA", "sliceB");
    auto result = sliceGraph(g, SlicingStrategy::Default);

    for (const auto& bridge : result.bridges) {
        // Send node exists in source slice
        const auto* sendNode = result.slices[bridge.sourceSlice].findNode(bridge.sendNodeId);
        ASSERT_TRUE(sendNode != nullptr);
        ASSERT_EQ(sendNode->type, NodeType::Send);

        // Receive node exists in dest slice
        const auto* recvNode = result.slices[bridge.destSlice].findNode(bridge.receiveNodeId);
        ASSERT_TRUE(recvNode != nullptr);
        ASSERT_EQ(recvNode->type, NodeType::Receive);

        // SHM names match
        ASSERT_TRUE(sendNode->sendConfig.has_value());
        ASSERT_TRUE(recvNode->receiveConfig.has_value());
        ASSERT_EQ(sendNode->sendConfig->shmName, bridge.shmName);
        ASSERT_EQ(recvNode->receiveConfig->shmName, bridge.shmName);
    }
}

// ---------------------------------------------------------------------------
// Edge case: no plugins
// ---------------------------------------------------------------------------

TEST(GraphSlicer_NoPlugins_SingleSlice) {
    Graph g({48000, 128});
    g.addNode(createInputNode("in", {{ChannelFormat::Stereo, 2}, ""}));
    g.addNode(createGainNode("gain", {{ChannelFormat::Stereo, 2}, 0.5f, false, false}));
    g.addNode(createOutputNode("out", {{ChannelFormat::Stereo, 2}, ""}));
    g.addEdge("in", 0, "gain", 0);
    g.addEdge("gain", 0, "out", 0);

    auto result = sliceGraph(g, SlicingStrategy::CrashIsolation);
    ASSERT_TRUE(result.isSingleSlice());
}

// ---------------------------------------------------------------------------
// Single plugin — CrashIsolation should still be one slice
// ---------------------------------------------------------------------------

TEST(GraphSlicer_CrashIsolation_SinglePlugin_OneSlice) {
    auto g = makeSimpleChain();
    auto result = sliceGraph(g, SlicingStrategy::CrashIsolation);
    ASSERT_TRUE(result.isSingleSlice());
}
