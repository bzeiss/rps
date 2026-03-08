#include "TestFramework.hpp"
#include <rps/coordinator/GraphSerializer.hpp>
#include <rps/coordinator/Graph.hpp>
#include <rps/coordinator/GraphNode.hpp>
#include <rps/coordinator/ChannelFormat.hpp>

using namespace rps::coordinator;

namespace {

ChannelLayout stereoLayout() {
    return {ChannelFormat::Stereo, 2};
}

Graph buildTestGraph() {
    Graph g(Graph::Config{44100, 256});
    g.addNode(createInputNode("in", {stereoLayout(), "shm_input"}));
    g.addNode(createGainNode("g", {stereoLayout(), 0.75f, false, false}));
    g.addNode(createOutputNode("out", {stereoLayout(), "shm_output"}));
    g.addEdge("in", 0, "g", 0);
    g.addEdge("g", 0, "out", 0);
    return g;
}

} // anonymous namespace

TEST(Serializer_RoundTrip_SimpleChain) {
    auto original = buildTestGraph();
    auto json = GraphSerializer::toJson(original);
    auto restored = GraphSerializer::fromJson(json);

    ASSERT_EQ(restored.config().sampleRate, 44100u);
    ASSERT_EQ(restored.config().blockSize, 256u);
    ASSERT_EQ(restored.nodeCount(), 3u);
    ASSERT_EQ(restored.edges().size(), 2u);

    auto* inNode = restored.findNode("in");
    ASSERT_TRUE(inNode != nullptr);
    ASSERT_EQ(static_cast<uint32_t>(inNode->type), static_cast<uint32_t>(NodeType::Input));

    auto* gNode = restored.findNode("g");
    ASSERT_TRUE(gNode != nullptr);
    ASSERT_EQ(static_cast<uint32_t>(gNode->type), static_cast<uint32_t>(NodeType::Gain));
    ASSERT_TRUE(gNode->gainConfig.has_value());
    ASSERT_NEAR(gNode->gainConfig->gain, 0.75f, 0.001f);
}

TEST(Serializer_RoundTrip_PluginNode) {
    Graph g(Graph::Config{48000, 128});
    g.addNode(createInputNode("in", {stereoLayout(), ""}));
    auto plugin = createPluginNode("eq", {"C:/VST3/FabFilter.vst3", "vst3"}, stereoLayout());
    plugin.latencySamples = 512;
    g.addNode(std::move(plugin));
    g.addNode(createOutputNode("out", {stereoLayout(), ""}));
    g.addEdge("in", 0, "eq", 0);
    g.addEdge("eq", 0, "out", 0);

    auto json = GraphSerializer::toJson(g);
    auto restored = GraphSerializer::fromJson(json);

    auto* eq = restored.findNode("eq");
    ASSERT_TRUE(eq != nullptr);
    ASSERT_TRUE(eq->pluginConfig.has_value());
    ASSERT_EQ(eq->pluginConfig->pluginPath, "C:/VST3/FabFilter.vst3");
    ASSERT_EQ(eq->pluginConfig->format, "vst3");
    ASSERT_EQ(eq->latencySamples, 512u);
}

TEST(Serializer_RoundTrip_MixerNode) {
    Graph g(Graph::Config{48000, 128});
    g.addNode(createInputNode("in1", {stereoLayout(), ""}));
    g.addNode(createInputNode("in2", {stereoLayout(), ""}));
    MixerNodeConfig mixCfg;
    mixCfg.outputLayout = stereoLayout();
    mixCfg.numInputs = 2;
    mixCfg.inputGains = {0.6f, 0.4f};
    g.addNode(createMixerNode("mix", mixCfg));
    g.addNode(createOutputNode("out", {stereoLayout(), ""}));
    g.addEdge("in1", 0, "mix", 0);
    g.addEdge("in2", 0, "mix", 1);
    g.addEdge("mix", 0, "out", 0);

    auto json = GraphSerializer::toJson(g);
    auto restored = GraphSerializer::fromJson(json);

    auto* mix = restored.findNode("mix");
    ASSERT_TRUE(mix != nullptr);
    ASSERT_TRUE(mix->mixerConfig.has_value());
    ASSERT_EQ(mix->mixerConfig->numInputs, 2u);
    ASSERT_NEAR(mix->mixerConfig->inputGains[0], 0.6f, 0.01f);
    ASSERT_NEAR(mix->mixerConfig->inputGains[1], 0.4f, 0.01f);
}

TEST(Serializer_RoundTrip_ChannelRouter) {
    Graph g(Graph::Config{48000, 128});
    ChannelLayout quad{ChannelFormat::Quad, 4};
    g.addNode(createInputNode("in", {{ChannelFormat::Quad, 4}, ""}));
    ChannelRouterNodeConfig routerCfg;
    routerCfg.inputLayout = quad;
    routerCfg.outputLayout = stereoLayout();
    routerCfg.routing = {0, 1}; // Take first two channels
    g.addNode(createChannelRouterNode("router", routerCfg));
    g.addNode(createOutputNode("out", {stereoLayout(), ""}));
    g.addEdge("in", 0, "router", 0);
    g.addEdge("router", 0, "out", 0);

    auto json = GraphSerializer::toJson(g);
    auto restored = GraphSerializer::fromJson(json);

    auto* router = restored.findNode("router");
    ASSERT_TRUE(router != nullptr);
    ASSERT_TRUE(router->channelRouterConfig.has_value());
    ASSERT_EQ(router->channelRouterConfig->routing.size(), 2u);
    ASSERT_EQ(router->channelRouterConfig->routing[0], 0u);
    ASSERT_EQ(router->channelRouterConfig->routing[1], 1u);
}

TEST(Serializer_InvalidJson_Throws) {
    ASSERT_THROWS(GraphSerializer::fromJson("not valid json"));
}

TEST(Serializer_MissingGraphKey_Throws) {
    ASSERT_THROWS(GraphSerializer::fromJson(R"({"version":1})"));
}

TEST(Serializer_RestoredGraphIsValid) {
    auto original = buildTestGraph();
    auto json = GraphSerializer::toJson(original);
    auto restored = GraphSerializer::fromJson(json);
    auto result = restored.validate();
    ASSERT_TRUE(result.valid);
}
