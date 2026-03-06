#pragma once

#include <rps/gui/IPluginGuiHost.hpp>
#include <rps/gui/SdlWindow.hpp>

#include <vector>

// Forward declarations
struct clap_plugin;
struct clap_plugin_entry;
struct clap_plugin_gui;
struct clap_plugin_params;
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

    /// Called by the hostGuiRequestResize callback when the plugin requests a resize.
    void onPluginRequestResize(uint32_t width, uint32_t height);

private:
    // CLAP plugin state
    void* m_libHandle = nullptr;
    const clap_plugin_entry* m_entry = nullptr;
    const clap_plugin* m_plugin = nullptr;
    const clap_plugin_gui* m_gui = nullptr;
    const clap_plugin_params* m_params = nullptr;

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

    void cleanup();
};

} // namespace rps::scanner
