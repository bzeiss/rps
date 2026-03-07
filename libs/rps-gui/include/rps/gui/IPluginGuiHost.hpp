#pragma once

#include <rps/ipc/Messages.hpp>

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
// IPluginGuiHost interface
// ---------------------------------------------------------------------------

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

