#pragma once

#include <rps/gui/IPluginGuiHost.hpp>
#include <rps/gui/SdlWindow.hpp>

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
    bool m_canResize = false;
    bool m_inResize = false;  // re-entrancy guard for resize

#ifdef _WIN32
    // Dedicated child HWND for plugin view — isolates JUCE's WndProc
    // subclassing from SDL's internal WndProc to prevent stack overflow.
    HWND m_pluginHwnd = nullptr;
#endif

    void cleanup();
};

} // namespace rps::scanner
