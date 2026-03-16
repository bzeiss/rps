#pragma once

#include <rps/gui/IPluginGuiHost.hpp>
#include <rps/gui/SdlWindow.hpp>

#include <thread>
#include <mutex>
#include <atomic>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <imm.h>    // ImmAssociateContext — needed to disable IMM on plugin HWND
#pragma comment(lib, "imm32.lib")
#endif

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/funknownimpl.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "public.sdk/source/vst/hosting/module.h"

#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace rps::scanner {

/// VST3-specific implementation of IPluginGuiHost.
/// Loads a VST3 plugin bundle, creates an SDL3 window, and embeds the plugin's IPlugView.
class Vst3GuiHost : public rps::gui::IPluginGuiHost {
public:
    Vst3GuiHost();
    ~Vst3GuiHost() override;

    OpenResult open(const boost::filesystem::path& pluginPath) override;
    void loadPlugin(const boost::filesystem::path& pluginPath) override;
    void runEventLoop(
        std::function<void(const std::string& reason)> closedCb,
        std::function<void(std::vector<rps::gui::ParameterValueUpdate>)> paramChangeCb = nullptr) override;
    void requestClose() override;
    void destroyGui() override;
    std::string getPluginName() const override { return m_pluginName; }
    rps::v1::ParameterList getParameters() override;
    std::vector<rps::gui::ParameterValueUpdate> pollParameterChanges() override;
    rps::host::GetStateResult saveState() override;
    rps::host::SetStateResult loadState(const std::string& stateData) override;
    rps::v1::PresetList getPresets() override;
    rps::host::LoadPresetResult loadPreset(const std::string& presetId) override;

    /// Returns true if the async MetaInfo enrichment has new data available.
    bool hasEnrichedPresets() const override { return m_presetsEnriched.load(std::memory_order_relaxed); }

    /// Returns the enriched preset list and clears the flag.
    rps::v1::PresetList getEnrichedPresets() override;

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
    void setToolbarCallbacks(const rps::gui::ToolbarCallbacks& cb) override {
        m_window.setToolbarCallbacks(cb);
    }

    /// Called by our IPlugFrame implementation when the plugin requests a resize.
    void onPluginRequestResize(Steinberg::ViewRect* newSize);

private:
    // VST3 plugin state
    VST3::Hosting::Module::Ptr m_module;
    Steinberg::IPtr<Steinberg::Vst::IComponent> m_component;
    Steinberg::IPtr<Steinberg::Vst::IEditController> m_controller;
    Steinberg::IPtr<Steinberg::IPlugView> m_view;
    Steinberg::FUnknownPtr<Steinberg::Vst::IAudioProcessor> m_processor;

    // SDL Window
    rps::gui::SdlWindow m_window;

    // Host state
    std::string m_pluginName;
    std::string m_pluginVendor;
    Steinberg::FUID m_componentFUID;
    bool m_canResize = false;
    bool m_inPluginResize = false;  // true during plugin-initiated resize (resizeView)
    bool m_inOnSize = false;        // prevents resizeView recursion from within onSize

    // Parameter polling state — cached values for change detection
    struct CachedParam {
        std::string id;         // ParamID as string
        uint32_t paramId = 0;   // VST3 ParamID (uint32)
        double lastValue = 0.0; // Last known plain-scale value
    };
    std::vector<CachedParam> m_cachedParams;

    // Preset list (populated by getPresets)
    rps::v1::PresetList m_presets;
    std::mutex m_presetMutex;
    std::atomic<bool> m_presetsEnriched{false};
    std::thread m_presetEnrichThread;

    /// Background: parse .vstpreset MetaInfo chunks and update m_presets.
    void enrichPresetsFromFiles();

    // Audio processing state
    bool m_audioActive = false;
    uint32_t m_audioInputChannels = 0;
    uint32_t m_audioOutputChannels = 0;
    uint32_t m_audioBlockSize = 0;
    std::vector<std::vector<float>> m_inputChannelBuffers;
    std::vector<std::vector<float>> m_outputChannelBuffers;
    std::vector<float*> m_inputPtrs;   // Pointers into m_inputChannelBuffers
    std::vector<float*> m_outputPtrs;  // Pointers into m_outputChannelBuffers
    double m_sampleRate = 48000.0;

    // Multi-bus support (e.g., sidechain)
    uint32_t m_numInputBuses = 1;
    uint32_t m_numOutputBuses = 1;
    std::vector<std::vector<std::vector<float>>> m_extraInputBuses;
    std::vector<std::vector<float*>> m_extraInputBusPtrs;
    std::vector<std::vector<std::vector<float>>> m_extraOutputBuses;
    std::vector<std::vector<float*>> m_extraOutputBusPtrs;

    void cleanup();

    // IPlugFrame + IRunLoop (Linux) — opaque pointer to Vst3PlugFrame (defined in .cpp)
    void* m_plugFramePtr = nullptr;
};

} // namespace rps::scanner
