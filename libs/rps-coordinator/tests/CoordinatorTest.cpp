#include "TestFramework.hpp"
#include <rps/coordinator/Coordinator.hpp>
#include <rps/coordinator/GraphNode.hpp>
#include <rps/coordinator/GraphSerializer.hpp>

using namespace rps::coordinator;

// ---------------------------------------------------------------------------
// Create / Destroy lifecycle
// ---------------------------------------------------------------------------

TEST(Coordinator_CreateAndDestroy) {
    Coordinator coord;
    auto id = coord.createGraph({48000, 128});
    ASSERT_TRUE(!id.empty());
    auto info = coord.getGraphInfo(id);
    ASSERT_EQ(info.nodeCount, static_cast<uint32_t>(0));
    ASSERT_EQ(info.state, GraphState::Inactive);
    coord.destroyGraph(id);
    ASSERT_THROWS(coord.getGraphInfo(id));
}

// ---------------------------------------------------------------------------
// Node and edge management
// ---------------------------------------------------------------------------

TEST(Coordinator_AddNodesAndEdges) {
    Coordinator coord;
    auto gId = coord.createGraph({48000, 128});

    coord.addNode(gId, createInputNode("in", {{ChannelFormat::Stereo, 2}, ""}));
    coord.addNode(gId, createGainNode("gain", {{ChannelFormat::Stereo, 2}, 0.5f, false, false}));
    coord.addNode(gId, createOutputNode("out", {{ChannelFormat::Stereo, 2}, ""}));

    coord.connectNodes(gId, "in", 0, "gain", 0);
    coord.connectNodes(gId, "gain", 0, "out", 0);

    auto info = coord.getGraphInfo(gId);
    ASSERT_EQ(info.nodeCount, static_cast<uint32_t>(3));
    ASSERT_EQ(info.edgeCount, static_cast<uint32_t>(2));
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

TEST(Coordinator_Validate_ValidGraph) {
    Coordinator coord;
    auto gId = coord.createGraph({48000, 128});
    coord.addNode(gId, createInputNode("in", {{ChannelFormat::Stereo, 2}, ""}));
    coord.addNode(gId, createGainNode("gain", {{ChannelFormat::Stereo, 2}, 0.5f, false, false}));
    coord.addNode(gId, createOutputNode("out", {{ChannelFormat::Stereo, 2}, ""}));
    coord.connectNodes(gId, "in", 0, "gain", 0);
    coord.connectNodes(gId, "gain", 0, "out", 0);

    auto result = coord.validateGraph(gId);
    ASSERT_TRUE(result.valid);
}

TEST(Coordinator_Validate_NoOutput) {
    Coordinator coord;
    auto gId = coord.createGraph({48000, 128});
    coord.addNode(gId, createInputNode("in", {{ChannelFormat::Stereo, 2}, ""}));

    auto result = coord.validateGraph(gId);
    ASSERT_FALSE(result.valid);
}

// ---------------------------------------------------------------------------
// Activate / Deactivate (Performance = single slice, in-process)
// ---------------------------------------------------------------------------

TEST(Coordinator_ActivatePerformance) {
    Coordinator coord;
    auto gId = coord.createGraph({48000, 128});
    coord.addNode(gId, createInputNode("in", {{ChannelFormat::Stereo, 2}, ""}));
    coord.addNode(gId, createGainNode("gain", {{ChannelFormat::Stereo, 2}, 0.5f, false, false}));
    coord.addNode(gId, createOutputNode("out", {{ChannelFormat::Stereo, 2}, ""}));
    coord.connectNodes(gId, "in", 0, "gain", 0);
    coord.connectNodes(gId, "gain", 0, "out", 0);

    coord.activateGraph(gId, SlicingStrategy::Performance);
    auto info = coord.getGraphInfo(gId);
    ASSERT_EQ(info.state, GraphState::Active);
    ASSERT_EQ(info.sliceCount, static_cast<uint32_t>(1));

    // Should have an executor
    ASSERT_TRUE(coord.getExecutor(gId) != nullptr);

    coord.deactivateGraph(gId);
    info = coord.getGraphInfo(gId);
    ASSERT_EQ(info.state, GraphState::Inactive);
    ASSERT_TRUE(coord.getExecutor(gId) == nullptr);
}

// ---------------------------------------------------------------------------
// Cannot modify active graph
// ---------------------------------------------------------------------------

TEST(Coordinator_CannotModifyActiveGraph) {
    Coordinator coord;
    auto gId = coord.createGraph({48000, 128});
    coord.addNode(gId, createInputNode("in", {{ChannelFormat::Stereo, 2}, ""}));
    coord.addNode(gId, createGainNode("gain", {{ChannelFormat::Stereo, 2}, 0.5f, false, false}));
    coord.addNode(gId, createOutputNode("out", {{ChannelFormat::Stereo, 2}, ""}));
    coord.connectNodes(gId, "in", 0, "gain", 0);
    coord.connectNodes(gId, "gain", 0, "out", 0);
    coord.activateGraph(gId);

    ASSERT_THROWS(coord.addNode(gId, createGainNode("g2", {{ChannelFormat::Stereo, 2}, 1.0f, false, false})));
    ASSERT_THROWS(coord.removeNode(gId, "gain"));
    ASSERT_THROWS(coord.connectNodes(gId, "in", 0, "out", 0));

    coord.deactivateGraph(gId);
}

// ---------------------------------------------------------------------------
// Serialize / Deserialize round-trip
// ---------------------------------------------------------------------------

TEST(Coordinator_SerializeDeserialize) {
    Coordinator coord;
    auto gId = coord.createGraph({48000, 128});
    coord.addNode(gId, createInputNode("in", {{ChannelFormat::Stereo, 2}, ""}));
    coord.addNode(gId, createGainNode("gain", {{ChannelFormat::Stereo, 2}, 0.5f, false, false}));
    coord.addNode(gId, createOutputNode("out", {{ChannelFormat::Stereo, 2}, ""}));
    coord.connectNodes(gId, "in", 0, "gain", 0);
    coord.connectNodes(gId, "gain", 0, "out", 0);

    auto json = coord.serializeGraph(gId);
    ASSERT_TRUE(!json.empty());

    auto gId2 = coord.deserializeGraph(json);
    ASSERT_TRUE(!gId2.empty());
    ASSERT_TRUE(gId != gId2);

    auto info = coord.getGraphInfo(gId2);
    ASSERT_EQ(info.nodeCount, static_cast<uint32_t>(3));
    ASSERT_EQ(info.edgeCount, static_cast<uint32_t>(2));
}

// ---------------------------------------------------------------------------
// Multiple graphs
// ---------------------------------------------------------------------------

TEST(Coordinator_MultipleGraphs) {
    Coordinator coord;
    auto g1 = coord.createGraph({48000, 128});
    auto g2 = coord.createGraph({44100, 256});
    ASSERT_TRUE(g1 != g2);

    coord.addNode(g1, createInputNode("in", {{ChannelFormat::Stereo, 2}, ""}));
    coord.addNode(g2, createInputNode("in", {{ChannelFormat::Mono, 1}, ""}));

    auto info1 = coord.getGraphInfo(g1);
    auto info2 = coord.getGraphInfo(g2);
    ASSERT_EQ(info1.nodeCount, static_cast<uint32_t>(1));
    ASSERT_EQ(info2.nodeCount, static_cast<uint32_t>(1));

    coord.destroyGraph(g1);
    coord.destroyGraph(g2);
}
