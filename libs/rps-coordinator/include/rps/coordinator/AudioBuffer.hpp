#pragma once

#include <cstdint>
#include <vector>

namespace rps::coordinator {

// ---------------------------------------------------------------------------
// AudioBuffer — deinterleaved multi-channel audio buffer
// ---------------------------------------------------------------------------

/// Multi-channel audio buffer for internal graph routing.
/// Stored as deinterleaved channels (one contiguous float* per channel)
/// for efficient per-channel operations matching VST3/CLAP conventions.
class AudioBuffer {
public:
    AudioBuffer() = default;
    AudioBuffer(uint32_t numChannels, uint32_t blockSize);

    /// Get a writable pointer to a specific channel.
    float* channel(uint32_t ch);

    /// Get a read-only pointer to a specific channel.
    const float* channel(uint32_t ch) const;

    uint32_t numChannels() const { return m_numChannels; }
    uint32_t blockSize() const { return m_blockSize; }

    /// Returns true if the buffer has been allocated.
    bool isValid() const { return !m_data.empty(); }

    /// Clear all channels to zero.
    void clear();

    /// Copy all samples from another buffer (must have matching dimensions).
    void copyFrom(const AudioBuffer& other);

    /// Mix another buffer into this one using 64-bit double accumulation.
    /// Channels are summed: this[ch][i] += other[ch][i] * gain.
    /// Only mixes min(this.channels, other.channels) channels.
    void mixIn(const AudioBuffer& other, float gain = 1.0f);

    /// Apply a gain factor to all channels.
    void applyGain(float gain);

    /// Deinterleave from interleaved float array into this buffer.
    /// interleaved layout: [ch0_s0, ch1_s0, ..., ch0_s1, ch1_s1, ...]
    void deinterleaveFrom(const float* interleaved, uint32_t numChannels, uint32_t numSamples);

    /// Interleave this buffer into a flat float array.
    void interleaveTo(float* interleaved) const;

private:
    uint32_t m_numChannels = 0;
    uint32_t m_blockSize = 0;
    std::vector<float> m_data; // Flat storage: [ch0_samples..., ch1_samples..., ...]
};

} // namespace rps::coordinator
