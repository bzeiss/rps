#pragma once

#include <rps/gui/IPluginGuiHost.hpp>
#include <rps/gui/SdlWindow.hpp>

#include <vector>

// Forward declarations
struct clap_plugin;
struct clap_plugin_entry;
struct clap_plugin_gui;
struct clap_plugin_params;
struct clap_plugin_state;
struct clap_plugin_preset_load;
struct clap_plugin_audio_ports;
struct clap_plugin_latency;
struct clap_host;

namespace rps::scanner {

/// CLAP-specific implementation of IPluginGuiHost.
/// Loads a CLAP plugin, creates an SDL3 window, and embeds the plugin's native GUI.
class ClapGuiHost : public rps::gui::IPluginGuiHost {
public:
    ClapGuiHost();
    ~ClapGuiHost() override;

    OpenResult open(const boost::filesystem::path& pluginPath) override;
    void runEventLoop(
        std::function<void(const std::string& reason)> closedCb,
        std::function<void(std::vector<rps::ipc::ParameterValueUpdate>)> paramChangeCb = nullptr) override;
    void requestClose() override;
    std::vector<rps::ipc::PluginParameterInfo> getParameters() override;
    std::vector<rps::ipc::ParameterValueUpdate> pollParameterChanges() override;
    rps::ipc::GetStateResponse saveState() override;
    rps::ipc::SetStateResponse loadState(const std::vector<uint8_t>& stateData) override;
    std::vector<rps::ipc::PresetInfo> getPresets() override;
    rps::ipc::LoadPresetResponse loadPreset(const std::string& presetId) override;

    // Audio processing overrides
    bool supportsAudioProcessing() const override { return true; }
    std::optional<rps::gui::AudioBusLayout> setupAudioProcessing(
        uint32_t sampleRate, uint32_t blockSize, uint32_t numChannels) override;
    bool processAudioBlock(
        const float* input, float* output,
        uint32_t numInputChannels, uint32_t numOutputChannels, uint32_t numSamples,
        const std::vector<rps::gui::AutomationEvent>& automation = {}) override;
    uint32_t getLatencySamples() const override;
    void teardownAudioProcessing() override;

    /// Called by the hostGuiRequestResize callback when the plugin requests a resize.
    void onPluginRequestResize(uint32_t width, uint32_t height);

private:
    // CLAP plugin state
    void* m_libHandle = nullptr;
    const clap_plugin_entry* m_entry = nullptr;
    const clap_plugin* m_plugin = nullptr;
    const clap_plugin_gui* m_gui = nullptr;
    const clap_plugin_params* m_params = nullptr;
    const clap_plugin_state* m_state = nullptr;

    // SDL Window
    rps::gui::SdlWindow m_window;

    // Host callbacks storage
    std::string m_pluginName;
    bool m_guiCreated = false;
    bool m_canResize = false;

    // Parameter polling state — cached values for change detection
    struct CachedParam {
        std::string id;
        double lastValue = 0.0;
    };
    std::vector<CachedParam> m_cachedParams;

    // Preset discovery
    const clap_plugin_preset_load* m_presetLoad = nullptr;
    std::vector<rps::ipc::PresetInfo> m_presets;

    void cleanup();
    void discoverPresets();  // Crawl CLAP preset discovery factory

    // Audio processing state
    const clap_plugin_audio_ports* m_audioPorts = nullptr;
    const clap_plugin_latency* m_latencyExt = nullptr;
    bool m_audioActive = false;
    uint32_t m_audioInputChannels = 0;
    uint32_t m_audioOutputChannels = 0;
    uint32_t m_audioBlockSize = 0;
    std::vector<std::vector<float>> m_inputChannelBuffers;
    std::vector<std::vector<float>> m_outputChannelBuffers;
    std::vector<float*> m_inputPtrs;
    std::vector<float*> m_outputPtrs;
};

} // namespace rps::scanner
