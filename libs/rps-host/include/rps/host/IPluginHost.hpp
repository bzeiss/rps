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

namespace rps::host {

// ---------------------------------------------------------------------------
// Audio processing types
// ---------------------------------------------------------------------------

/// Sample-accurate automation event (extension point for future DAW automation).
struct AutomationEvent {
    uint32_t sampleOffset = 0;
    std::string paramId;
    double value = 0.0;
};

/// Negotiated bus layout reported back after setupAudioProcessing().
struct AudioBusLayout {
    uint32_t numInputChannels = 0;
    uint32_t numOutputChannels = 0;
};

// ---------------------------------------------------------------------------
// IPluginHost — format-agnostic plugin host interface
// ---------------------------------------------------------------------------

/// Result of loading a plugin (no GUI involved).
struct LoadResult {
    std::string name;
    std::string vendor;
    bool hasGui = false;
    bool canResize = false;
};

/// GUI editor size.
struct GuiSize {
    uint32_t width = 800;
    uint32_t height = 600;
};

/// Abstract interface for format-specific plugin hosts.
/// Covers the full lifecycle: load, audio processing, state, presets, and
/// optional GUI.  For headless use, simply never call openGui()/closeGui().
class IPluginHost {
public:
    virtual ~IPluginHost() = default;

    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------

    /// Load the plugin from disk. Does NOT open a GUI.
    /// @param pluginPath  Filesystem path to the plugin (.vst3, .clap, …).
    /// @return LoadResult with plugin metadata.
    /// @throws std::runtime_error on failure.
    virtual LoadResult load(const boost::filesystem::path& pluginPath) = 0;

    /// Unload the plugin and release all resources.
    virtual void unload() = 0;

    // -----------------------------------------------------------------------
    // GUI (optional — skip entirely for headless usage)
    // -----------------------------------------------------------------------

    /// Create and attach the plugin's editor inside a parent window.
    /// @param parentWindowHandle  Native window handle (HWND on Windows, etc.).
    /// @return The initial editor size.
    virtual GuiSize openGui(void* parentWindowHandle) = 0;

    /// Detach and destroy the plugin's editor.  Processing continues.
    virtual void closeGui() = 0;

    // -----------------------------------------------------------------------
    // Parameters
    // -----------------------------------------------------------------------

    /// Get the full list of parameters.
    virtual std::vector<rps::ipc::PluginParameterInfo> getParameters() = 0;

    /// Poll for parameter value changes since the last call.
    virtual std::vector<rps::ipc::ParameterValueUpdate> pollParameterChanges() = 0;

    // -----------------------------------------------------------------------
    // State
    // -----------------------------------------------------------------------

    virtual rps::ipc::GetStateResponse saveState() = 0;
    virtual rps::ipc::SetStateResponse loadState(const std::vector<uint8_t>& stateData) = 0;

    // -----------------------------------------------------------------------
    // Presets
    // -----------------------------------------------------------------------

    virtual std::vector<rps::ipc::PresetInfo> getPresets() = 0;
    virtual rps::ipc::LoadPresetResponse loadPreset(const std::string& presetId) = 0;

    /// Returns true if async preset metadata enrichment has new data.
    virtual bool hasEnrichedPresets() const { return false; }

    /// Returns the enriched preset list and clears the flag.
    virtual std::vector<rps::ipc::PresetInfo> getEnrichedPresets() { return {}; }

    // -----------------------------------------------------------------------
    // Audio processing
    // -----------------------------------------------------------------------

    /// Whether this host implementation supports audio processing.
    virtual bool supportsAudioProcessing() const { return false; }

    /// Negotiate bus layout and activate audio processing.
    virtual std::optional<AudioBusLayout> setupAudioProcessing(
        uint32_t /*sampleRate*/, uint32_t /*blockSize*/, uint32_t /*numChannels*/) {
        return std::nullopt;
    }

    /// Process one block of interleaved float32 audio.
    virtual bool processAudioBlock(
        const float* /*input*/, float* /*output*/,
        uint32_t /*numInputChannels*/, uint32_t /*numOutputChannels*/,
        uint32_t /*numSamples*/,
        const std::vector<AutomationEvent>& /*automation*/ = {}) {
        return false;
    }

    /// Report the plugin's processing latency in samples.
    virtual uint32_t getLatencySamples() const { return 0; }

    /// Deactivate audio processing and release audio resources.
    virtual void teardownAudioProcessing() {}
};

} // namespace rps::host
