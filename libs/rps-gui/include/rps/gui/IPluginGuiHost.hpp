#pragma once

#include <host.pb.h>
#include <rps.pb.h>

#include <string>
#include <cstdint>
#include <optional>
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

// ---------------------------------------------------------------------------
// Audio processing support types
// ---------------------------------------------------------------------------

/// Sample-accurate automation event (extension point for future DAW automation).
/// In Phase 1 the automation vector is always empty.
struct AutomationEvent {
    uint32_t sampleOffset = 0;   // Offset within the current block
    std::string paramId;
    double value = 0.0;
};

/// Negotiated bus layout reported back after setupAudioProcessing().
/// May differ from the requested channel count if the plugin only supports
/// certain configurations (e.g. mono-in / stereo-out).
struct AudioBusLayout {
    uint32_t numInputChannels = 0;
    uint32_t numOutputChannels = 0;
};

// ---------------------------------------------------------------------------
// Parameter value update (used for delta streaming callback)
// ---------------------------------------------------------------------------
struct ParameterValueUpdate {
    std::string paramId;
    double value = 0.0;
    std::string displayText;
};

// ---------------------------------------------------------------------------
// IPluginGuiHost interface
// ---------------------------------------------------------------------------

// ToolbarCallbacks is defined in SdlWindow.hpp. Forward-declare here so hosts
// that don't use SdlWindow can still compile the default no-op.
struct ToolbarCallbacks;

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

    /// Load the plugin from disk WITHOUT creating a GUI.
    /// Sets up the plugin for audio processing, state, presets, etc.
    /// @param pluginPath Filesystem path to the plugin binary.
    /// @throws std::runtime_error on failure.
    virtual void loadPlugin(const boost::filesystem::path& pluginPath) = 0;

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
        std::function<void(std::vector<ParameterValueUpdate>)> paramChangeCb = nullptr) = 0;

    /// Request the GUI to close from another thread (e.g. IPC command).
    virtual void requestClose() = 0;

    /// Tear down the GUI window and plugin GUI extensions, but keep the plugin loaded.
    /// Called when returning from GUI mode to headless mode.
    virtual void destroyGui() {}

    /// Get the loaded plugin's display name.
    virtual std::string getPluginName() const = 0;

    /// Get the full list of parameters as a protobuf ParameterList.
    virtual rps::v1::ParameterList getParameters() = 0;

    /// Poll for parameter value changes since the last call.
    /// Returns only parameters whose values have changed.
    virtual std::vector<ParameterValueUpdate> pollParameterChanges() = 0;

    /// Save the complete plugin state as an opaque binary blob.
    virtual rps::host::GetStateResult saveState() = 0;

    /// Load plugin state from a previously saved binary blob.
    virtual rps::host::SetStateResult loadState(const std::string& stateData) = 0;

    /// Get available presets/programs for this plugin.
    virtual rps::v1::PresetList getPresets() = 0;

    /// Load a preset by its id.
    virtual rps::host::LoadPresetResult loadPreset(const std::string& presetId) = 0;

    /// Returns true if async preset metadata enrichment has completed with new data.
    /// Default returns false (no async enrichment). Override in hosts that support it.
    virtual bool hasEnrichedPresets() const { return false; }

    /// Returns the enriched preset list and clears the flag.
    virtual rps::v1::PresetList getEnrichedPresets() { return {}; }

    /// Set toolbar button callbacks (bypass, delta, etc.).
    /// Default no-op. Override in hosts that use SdlWindow's toolbar.
    virtual void setToolbarCallbacks(const ToolbarCallbacks& /*cb*/) {}

    // -----------------------------------------------------------------------
    // Audio processing interface (Phase 1)
    // All methods have default no-op implementations so existing GUI-only
    // hosts continue to work without modification.
    // -----------------------------------------------------------------------

    /// Whether this host implementation supports audio processing.
    virtual bool supportsAudioProcessing() const { return false; }

    /// Negotiate bus layout and activate audio processing.
    /// The plugin may accept a different channel count than requested.
    /// @return The negotiated layout, or nullopt on failure.
    virtual std::optional<AudioBusLayout> setupAudioProcessing(
        uint32_t /*sampleRate*/, uint32_t /*blockSize*/, uint32_t /*numChannels*/) {
        return std::nullopt;
    }

    /// Process one block of interleaved float32 audio.
    /// Input layout: numInputChannels interleaved samples × numSamples.
    /// Output layout: numOutputChannels interleaved samples × numSamples.
    /// @param automation Sample-accurate parameter changes (empty in Phase 1).
    virtual bool processAudioBlock(
        const float* /*input*/, float* /*output*/,
        uint32_t /*numInputChannels*/, uint32_t /*numOutputChannels*/,
        uint32_t /*numSamples*/,
        const std::vector<AutomationEvent>& /*automation*/ = {}) {
        return false;
    }

    /// Report the plugin's processing latency in samples (for future PDC).
    virtual uint32_t getLatencySamples() const { return 0; }

    /// Deactivate audio processing and release audio resources.
    virtual void teardownAudioProcessing() {}
};

} // namespace rps::gui
