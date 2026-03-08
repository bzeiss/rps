#include <rps/coordinator/AudioBuffer.hpp>

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace rps::coordinator {

AudioBuffer::AudioBuffer(uint32_t numChannels, uint32_t blockSize)
    : m_numChannels(numChannels)
    , m_blockSize(blockSize)
    , m_data(static_cast<size_t>(numChannels) * blockSize, 0.0f) {}

float* AudioBuffer::channel(uint32_t ch) {
    if (ch >= m_numChannels) {
        throw std::out_of_range("AudioBuffer::channel() index out of range");
    }
    return m_data.data() + static_cast<size_t>(ch) * m_blockSize;
}

const float* AudioBuffer::channel(uint32_t ch) const {
    if (ch >= m_numChannels) {
        throw std::out_of_range("AudioBuffer::channel() index out of range");
    }
    return m_data.data() + static_cast<size_t>(ch) * m_blockSize;
}

void AudioBuffer::clear() {
    std::fill(m_data.begin(), m_data.end(), 0.0f);
}

void AudioBuffer::copyFrom(const AudioBuffer& other) {
    if (other.m_numChannels != m_numChannels || other.m_blockSize != m_blockSize) {
        throw std::invalid_argument("AudioBuffer::copyFrom() dimension mismatch");
    }
    std::memcpy(m_data.data(), other.m_data.data(), m_data.size() * sizeof(float));
}

void AudioBuffer::mixIn(const AudioBuffer& other, float gain) {
    uint32_t channels = std::min(m_numChannels, other.m_numChannels);
    uint32_t samples = std::min(m_blockSize, other.m_blockSize);

    for (uint32_t ch = 0; ch < channels; ++ch) {
        float* dst = channel(ch);
        const float* src = other.channel(ch);
        for (uint32_t i = 0; i < samples; ++i) {
            // Accumulate in double for precision, then store back as float
            double acc = static_cast<double>(dst[i])
                       + static_cast<double>(src[i]) * static_cast<double>(gain);
            dst[i] = static_cast<float>(acc);
        }
    }
}

void AudioBuffer::applyGain(float gain) {
    for (auto& sample : m_data) {
        sample *= gain;
    }
}

void AudioBuffer::deinterleaveFrom(const float* interleaved, uint32_t numChannels,
                                    uint32_t numSamples) {
    if (numChannels != m_numChannels || numSamples != m_blockSize) {
        throw std::invalid_argument("AudioBuffer::deinterleaveFrom() dimension mismatch");
    }
    for (uint32_t s = 0; s < numSamples; ++s) {
        for (uint32_t ch = 0; ch < numChannels; ++ch) {
            channel(ch)[s] = interleaved[s * numChannels + ch];
        }
    }
}

void AudioBuffer::interleaveTo(float* interleaved) const {
    for (uint32_t s = 0; s < m_blockSize; ++s) {
        for (uint32_t ch = 0; ch < m_numChannels; ++ch) {
            interleaved[s * m_numChannels + ch] = channel(ch)[s];
        }
    }
}

} // namespace rps::coordinator
