#pragma once

#include <rps/ipc/Messages.hpp>

#include <string>
#include <cstdint>
#include <vector>
#include <functional>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4100)
#endif
#include <boost/filesystem.hpp>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace rps::gui {

/// Abstract interface for format-specific plugin GUI hosts.
/// Each format (CLAP, VST3, etc.) implements this interface in its own binary.
class IPluginGuiHost {
public:
    virtual ~IPluginGuiHost() = default;

    struct OpenResult {
        std::string name;
        uint32_t width = 800;
        uint32_t height = 600;
    };

    /// Load the plugin and prepare its GUI for display.
    /// @param pluginPath Filesystem path to the plugin binary.
    /// @return OpenResult with plugin name and initial GUI dimensions.
    /// @throws std::runtime_error on failure.
    virtual OpenResult open(const boost::filesystem::path& pluginPath) = 0;

    /// Run the GUI event loop. Blocks until the window is closed or requestClose() is called.
    /// @param closedCb Called with a reason string ("user", "host", "crash") when the loop exits.
    /// @param paramChangeCb Called with delta parameter updates (~20Hz) during the loop.
    virtual void runEventLoop(
        std::function<void(const std::string& reason)> closedCb,
        std::function<void(std::vector<rps::ipc::ParameterValueUpdate>)> paramChangeCb = nullptr) = 0;

    /// Request the GUI to close from another thread (e.g. IPC command).
    virtual void requestClose() = 0;

    /// Get the full list of parameters. Called once after the plugin GUI is opened.
    virtual std::vector<rps::ipc::PluginParameterInfo> getParameters() = 0;

    /// Poll for parameter value changes since the last call.
    /// Returns only parameters whose values have changed.
    virtual std::vector<rps::ipc::ParameterValueUpdate> pollParameterChanges() = 0;

    /// Save the complete plugin state as an opaque binary blob.
    virtual rps::ipc::GetStateResponse saveState() = 0;

    /// Load plugin state from a previously saved binary blob.
    virtual rps::ipc::SetStateResponse loadState(const std::vector<uint8_t>& stateData) = 0;

    /// Get available presets/programs for this plugin.
    virtual std::vector<rps::ipc::PresetInfo> getPresets() = 0;

    /// Load a preset by its id.
    virtual rps::ipc::LoadPresetResponse loadPreset(const std::string& presetId) = 0;

    /// Returns true if async preset metadata enrichment has completed with new data.
    /// Default returns false (no async enrichment). Override in hosts that support it.
    virtual bool hasEnrichedPresets() const { return false; }

    /// Returns the enriched preset list and clears the flag.
    virtual std::vector<rps::ipc::PresetInfo> getEnrichedPresets() { return {}; }
};

} // namespace rps::gui
