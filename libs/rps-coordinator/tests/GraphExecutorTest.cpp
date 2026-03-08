#include "TestFramework.hpp"
#include <rps/coordinator/GraphExecutor.hpp>
#include <rps/coordinator/Graph.hpp>
#include <rps/coordinator/GraphNode.hpp>
#include <rps/coordinator/AudioBuffer.hpp>

using namespace rps::coordinator;

namespace {

ChannelLayout stereoLayout() {
    return {ChannelFormat::Stereo, 2};
}

constexpr uint32_t kBlockSize = 8;

AudioBuffer makeTestBuffer(float value) {
    AudioBuffer buf(2, kBlockSize);
    for (uint32_t ch = 0; ch < 2; ++ch) {
        for (uint32_t i = 0; i < kBlockSize; ++i) {
            buf.channel(ch)[i] = value;
        }
    }
    return buf;
}

} // anonymous namespace

TEST(GraphExecutor_Passthrough) {
    // input -> output (direct passthrough)
    Graph g(Graph::Config{48000, kBlockSize});
    g.addNode(createInputNode("in", {stereoLayout(), ""}));
    g.addNode(createOutputNode("out", {stereoLayout(), ""}));
    g.addEdge("in", 0, "out", 0);

    GraphExecutor exec;
    exec.prepare(g);

    std::unordered_map<std::string, AudioBuffer> inputs;
    inputs.emplace("in", makeTestBuffer(1.0f));

    std::unordered_map<std::string, AudioBuffer> outputs;
    exec.processBlock(inputs, outputs);

    ASSERT_TRUE(outputs.contains("out"));
    for (uint32_t ch = 0; ch < 2; ++ch) {
        for (uint32_t i = 0; i < kBlockSize; ++i) {
            ASSERT_NEAR(outputs["out"].channel(ch)[i], 1.0f, 1e-6);
        }
    }
}

TEST(GraphExecutor_GainNode) {
    // input -> gain(0.5) -> output
    Graph g(Graph::Config{48000, kBlockSize});
    g.addNode(createInputNode("in", {stereoLayout(), ""}));
    g.addNode(createGainNode("gain", {stereoLayout(), 0.5f, false, false}));
    g.addNode(createOutputNode("out", {stereoLayout(), ""}));
    g.addEdge("in", 0, "gain", 0);
    g.addEdge("gain", 0, "out", 0);

    GraphExecutor exec;
    exec.prepare(g);

    std::unordered_map<std::string, AudioBuffer> inputs;
    inputs.emplace("in", makeTestBuffer(1.0f));

    std::unordered_map<std::string, AudioBuffer> outputs;
    exec.processBlock(inputs, outputs);

    ASSERT_TRUE(outputs.contains("out"));
    for (uint32_t ch = 0; ch < 2; ++ch) {
        for (uint32_t i = 0; i < kBlockSize; ++i) {
            ASSERT_NEAR(outputs["out"].channel(ch)[i], 0.5f, 1e-6);
        }
    }
}

TEST(GraphExecutor_GainNode_Mute) {
    Graph g(Graph::Config{48000, kBlockSize});
    g.addNode(createInputNode("in", {stereoLayout(), ""}));
    g.addNode(createGainNode("gain", {stereoLayout(), 1.0f, true, false})); // mute=true
    g.addNode(createOutputNode("out", {stereoLayout(), ""}));
    g.addEdge("in", 0, "gain", 0);
    g.addEdge("gain", 0, "out", 0);

    GraphExecutor exec;
    exec.prepare(g);

    std::unordered_map<std::string, AudioBuffer> inputs;
    inputs.emplace("in", makeTestBuffer(1.0f));

    std::unordered_map<std::string, AudioBuffer> outputs;
    exec.processBlock(inputs, outputs);

    for (uint32_t ch = 0; ch < 2; ++ch) {
        for (uint32_t i = 0; i < kBlockSize; ++i) {
            ASSERT_NEAR(outputs["out"].channel(ch)[i], 0.0f, 1e-6);
        }
    }
}

TEST(GraphExecutor_GainNode_Bypass) {
    Graph g(Graph::Config{48000, kBlockSize});
    g.addNode(createInputNode("in", {stereoLayout(), ""}));
    g.addNode(createGainNode("gain", {stereoLayout(), 0.5f, false, true})); // bypass=true
    g.addNode(createOutputNode("out", {stereoLayout(), ""}));
    g.addEdge("in", 0, "gain", 0);
    g.addEdge("gain", 0, "out", 0);

    GraphExecutor exec;
    exec.prepare(g);

    std::unordered_map<std::string, AudioBuffer> inputs;
    inputs.emplace("in", makeTestBuffer(1.0f));

    std::unordered_map<std::string, AudioBuffer> outputs;
    exec.processBlock(inputs, outputs);

    // Bypass should pass audio through unmodified (gain = 0.5 but bypassed)
    for (uint32_t ch = 0; ch < 2; ++ch) {
        for (uint32_t i = 0; i < kBlockSize; ++i) {
            ASSERT_NEAR(outputs["out"].channel(ch)[i], 1.0f, 1e-6);
        }
    }
}

TEST(GraphExecutor_MixerNode_SumsInputs) {
    // in1(1.0) -> mixer -> output
    // in2(2.0) -> mixer -> output
    // Expected: 3.0 per sample
    Graph g(Graph::Config{48000, kBlockSize});
    g.addNode(createInputNode("in1", {stereoLayout(), ""}));
    g.addNode(createInputNode("in2", {stereoLayout(), ""}));
    MixerNodeConfig mixCfg;
    mixCfg.outputLayout = stereoLayout();
    mixCfg.numInputs = 2;
    g.addNode(createMixerNode("mix", mixCfg));
    g.addNode(createOutputNode("out", {stereoLayout(), ""}));
    g.addEdge("in1", 0, "mix", 0);
    g.addEdge("in2", 0, "mix", 1);
    g.addEdge("mix", 0, "out", 0);

    GraphExecutor exec;
    exec.prepare(g);

    std::unordered_map<std::string, AudioBuffer> inputs;
    inputs.emplace("in1", makeTestBuffer(1.0f));
    inputs.emplace("in2", makeTestBuffer(2.0f));

    std::unordered_map<std::string, AudioBuffer> outputs;
    exec.processBlock(inputs, outputs);

    for (uint32_t ch = 0; ch < 2; ++ch) {
        for (uint32_t i = 0; i < kBlockSize; ++i) {
            ASSERT_NEAR(outputs["out"].channel(ch)[i], 3.0f, 1e-6);
        }
    }
}

TEST(GraphExecutor_MixerNode_WithGains) {
    // in1(1.0) * 0.5 + in2(1.0) * 0.3 = 0.8
    Graph g(Graph::Config{48000, kBlockSize});
    g.addNode(createInputNode("in1", {stereoLayout(), ""}));
    g.addNode(createInputNode("in2", {stereoLayout(), ""}));
    MixerNodeConfig mixCfg;
    mixCfg.outputLayout = stereoLayout();
    mixCfg.numInputs = 2;
    mixCfg.inputGains = {0.5f, 0.3f};
    g.addNode(createMixerNode("mix", mixCfg));
    g.addNode(createOutputNode("out", {stereoLayout(), ""}));
    g.addEdge("in1", 0, "mix", 0);
    g.addEdge("in2", 0, "mix", 1);
    g.addEdge("mix", 0, "out", 0);

    GraphExecutor exec;
    exec.prepare(g);

    std::unordered_map<std::string, AudioBuffer> inputs;
    inputs.emplace("in1", makeTestBuffer(1.0f));
    inputs.emplace("in2", makeTestBuffer(1.0f));

    std::unordered_map<std::string, AudioBuffer> outputs;
    exec.processBlock(inputs, outputs);

    for (uint32_t ch = 0; ch < 2; ++ch) {
        for (uint32_t i = 0; i < kBlockSize; ++i) {
            ASSERT_NEAR(outputs["out"].channel(ch)[i], 0.8f, 1e-5);
        }
    }
}

TEST(GraphExecutor_FanOut_GainAndMixer) {
    // input(1.0) -> gain(0.5) -> mixer -> output
    // input(1.0) ------------>  mixer -> output
    // Expected: 0.5 + 1.0 = 1.5
    Graph g(Graph::Config{48000, kBlockSize});
    g.addNode(createInputNode("in", {stereoLayout(), ""}));
    g.addNode(createGainNode("gain", {stereoLayout(), 0.5f, false, false}));
    MixerNodeConfig mixCfg;
    mixCfg.outputLayout = stereoLayout();
    mixCfg.numInputs = 2;
    g.addNode(createMixerNode("mix", mixCfg));
    g.addNode(createOutputNode("out", {stereoLayout(), ""}));
    g.addEdge("in", 0, "gain", 0);
    g.addEdge("gain", 0, "mix", 0);
    g.addEdge("in", 0, "mix", 1);
    g.addEdge("mix", 0, "out", 0);

    GraphExecutor exec;
    exec.prepare(g);

    std::unordered_map<std::string, AudioBuffer> inputs;
    inputs.emplace("in", makeTestBuffer(1.0f));

    std::unordered_map<std::string, AudioBuffer> outputs;
    exec.processBlock(inputs, outputs);

    for (uint32_t ch = 0; ch < 2; ++ch) {
        for (uint32_t i = 0; i < kBlockSize; ++i) {
            ASSERT_NEAR(outputs["out"].channel(ch)[i], 1.5f, 1e-5);
        }
    }
}

TEST(GraphExecutor_PluginCallback) {
    // input -> plugin (doubles audio) -> output
    Graph g(Graph::Config{48000, kBlockSize});
    g.addNode(createInputNode("in", {stereoLayout(), ""}));
    g.addNode(createPluginNode("plugin", {"test.vst3", "vst3"}, stereoLayout()));
    g.addNode(createOutputNode("out", {stereoLayout(), ""}));
    g.addEdge("in", 0, "plugin", 0);
    g.addEdge("plugin", 0, "out", 0);

    GraphExecutor exec;
    exec.prepare(g, [](const std::string& /*nodeId*/,
                       const AudioBuffer& input,
                       AudioBuffer& output) -> bool {
        // Simple "plugin" that doubles the audio
        for (uint32_t ch = 0; ch < input.numChannels(); ++ch) {
            for (uint32_t i = 0; i < input.blockSize(); ++i) {
                output.channel(ch)[i] = input.channel(ch)[i] * 2.0f;
            }
        }
        return true;
    });

    std::unordered_map<std::string, AudioBuffer> inputs;
    inputs.emplace("in", makeTestBuffer(1.0f));

    std::unordered_map<std::string, AudioBuffer> outputs;
    exec.processBlock(inputs, outputs);

    for (uint32_t ch = 0; ch < 2; ++ch) {
        for (uint32_t i = 0; i < kBlockSize; ++i) {
            ASSERT_NEAR(outputs["out"].channel(ch)[i], 2.0f, 1e-6);
        }
    }
}

TEST(GraphExecutor_ChannelRouter) {
    // 4-channel input -> router (take channels 0,1) -> stereo output
    ChannelLayout quadLayout{ChannelFormat::Quad, 4};
    Graph g(Graph::Config{48000, kBlockSize});
    g.addNode(createInputNode("in", {quadLayout, ""}));
    ChannelRouterNodeConfig routerCfg;
    routerCfg.inputLayout = quadLayout;
    routerCfg.outputLayout = stereoLayout();
    routerCfg.routing = {0, 1};
    g.addNode(createChannelRouterNode("router", routerCfg));
    g.addNode(createOutputNode("out", {stereoLayout(), ""}));
    g.addEdge("in", 0, "router", 0);
    g.addEdge("router", 0, "out", 0);

    GraphExecutor exec;
    exec.prepare(g);

    AudioBuffer input(4, kBlockSize);
    for (uint32_t i = 0; i < kBlockSize; ++i) {
        input.channel(0)[i] = 1.0f;  // L
        input.channel(1)[i] = 2.0f;  // R
        input.channel(2)[i] = 3.0f;  // Ls
        input.channel(3)[i] = 4.0f;  // Rs
    }

    std::unordered_map<std::string, AudioBuffer> inputs;
    inputs.emplace("in", std::move(input));

    std::unordered_map<std::string, AudioBuffer> outputs;
    exec.processBlock(inputs, outputs);

    // Output should be channels 0 and 1 from the input
    for (uint32_t i = 0; i < kBlockSize; ++i) {
        ASSERT_NEAR(outputs["out"].channel(0)[i], 1.0f, 1e-6); // L
        ASSERT_NEAR(outputs["out"].channel(1)[i], 2.0f, 1e-6); // R
    }
}

TEST(GraphExecutor_TwoPluginsInSeries) {
    // input -> plugin_a (×2) -> plugin_b (×3) -> output
    // Expected: 1.0 × 2 × 3 = 6.0
    Graph g(Graph::Config{48000, kBlockSize});
    g.addNode(createInputNode("in", {stereoLayout(), ""}));
    g.addNode(createPluginNode("pA", {"a.vst3", "vst3"}, stereoLayout()));
    g.addNode(createPluginNode("pB", {"b.vst3", "vst3"}, stereoLayout()));
    g.addNode(createOutputNode("out", {stereoLayout(), ""}));
    g.addEdge("in", 0, "pA", 0);
    g.addEdge("pA", 0, "pB", 0);
    g.addEdge("pB", 0, "out", 0);

    GraphExecutor exec;
    exec.prepare(g, [](const std::string& nodeId,
                       const AudioBuffer& input,
                       AudioBuffer& output) -> bool {
        float multiplier = (nodeId == "pA") ? 2.0f : 3.0f;
        for (uint32_t ch = 0; ch < input.numChannels(); ++ch) {
            for (uint32_t i = 0; i < input.blockSize(); ++i) {
                output.channel(ch)[i] = input.channel(ch)[i] * multiplier;
            }
        }
        return true;
    });

    std::unordered_map<std::string, AudioBuffer> inputs;
    inputs.emplace("in", makeTestBuffer(1.0f));

    std::unordered_map<std::string, AudioBuffer> outputs;
    exec.processBlock(inputs, outputs);

    for (uint32_t ch = 0; ch < 2; ++ch) {
        for (uint32_t i = 0; i < kBlockSize; ++i) {
            ASSERT_NEAR(outputs["out"].channel(ch)[i], 6.0f, 1e-5);
        }
    }
}

TEST(GraphExecutor_NotPreparedThrows) {
    GraphExecutor exec;
    std::unordered_map<std::string, AudioBuffer> inputs, outputs;
    ASSERT_THROWS(exec.processBlock(inputs, outputs));
}
