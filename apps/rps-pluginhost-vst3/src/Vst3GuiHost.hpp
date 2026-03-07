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

    /// Returns true if the async MetaInfo enrichment has new data available.
    bool hasEnrichedPresets() const override { return m_presetsEnriched.load(std::memory_order_relaxed); }

    /// Returns the enriched preset list and clears the flag.
    std::vector<rps::ipc::PresetInfo> getEnrichedPresets() override;

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
    std::vector<rps::ipc::PresetInfo> m_presets;
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
};

} // namespace rps::scanner
