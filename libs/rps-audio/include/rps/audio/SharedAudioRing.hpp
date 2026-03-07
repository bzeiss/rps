#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4100)
#pragma warning(disable: 4244)
#pragma warning(disable: 4245)
#endif
#include <boost/interprocess/shared_memory_object.hpp>
#include <boost/interprocess/mapped_region.hpp>
#ifdef _WIN32
#include <boost/interprocess/windows_shared_memory.hpp>
#endif
#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace rps::audio {

// ---------------------------------------------------------------------------
// Shared memory layout — this struct lives at offset 0 of the shared segment.
// All fields before the atomics are written once at creation, read-only after.
// ---------------------------------------------------------------------------

struct AudioSegmentHeader {
    // Protocol version — increment on breaking changes
    uint32_t version = 1;

    // Audio format configuration (immutable after creation)
    uint32_t sampleRate = 48000;
    uint32_t blockSize = 128;          // Samples per block
    uint32_t numChannels = 2;          // Channel count (mono=1, stereo=2, etc.)
    uint32_t ringBlocks = 8;           // Number of blocks in each ring

    // === Extension points for full DAW (unused in Phase 1) ===
    uint32_t numSidechainInputs = 0;   // Future: sidechain bus count
    uint32_t numSendOutputs = 0;       // Future: send bus count
    uint32_t latencySamples = 0;       // Reported by plugin chain (future PDC)
    uint32_t flags = 0;               // Reserved (bypass, mute, etc.)

    // Padding to push atomics to their own cache lines
    uint8_t _reserved[20] = {};

    // SPSC lock-free positions — each on its own cache line to avoid false sharing.
    // Positions are monotonically increasing block indices (wrapping handled by modulo).
    alignas(64) std::atomic<uint64_t> inputWritePos{0};   // Producer: client (Python)
    alignas(64) std::atomic<uint64_t> inputReadPos{0};    // Consumer: plugin host
    alignas(64) std::atomic<uint64_t> outputWritePos{0};  // Producer: plugin host
    alignas(64) std::atomic<uint64_t> outputReadPos{0};   // Consumer: client (Python)

    // Transport state (future: tempo, time sig, sample position)
    alignas(64) std::atomic<uint32_t> transportState{0};  // 0=stopped, 1=playing
};

// ---------------------------------------------------------------------------
// SharedAudioRing — lock-free SPSC ring buffer over shared memory
// ---------------------------------------------------------------------------

class SharedAudioRing {
public:
    ~SharedAudioRing();

    // Non-copyable, non-movable (shared memory handles)
    SharedAudioRing(const SharedAudioRing&) = delete;
    SharedAudioRing& operator=(const SharedAudioRing&) = delete;
    SharedAudioRing(SharedAudioRing&&) = delete;
    SharedAudioRing& operator=(SharedAudioRing&&) = delete;

    /// Create a new shared memory segment (server/host side).
    /// The segment is created with the given audio parameters.
    static std::unique_ptr<SharedAudioRing> create(
        const std::string& name,
        uint32_t sampleRate,
        uint32_t blockSize,
        uint32_t numChannels,
        uint32_t ringBlocks = 8);

    /// Open an existing shared memory segment (client side).
    static std::unique_ptr<SharedAudioRing> open(const std::string& name);

    /// Remove a named shared memory segment (cleanup).
    static void remove(const std::string& name);

    // -- Input ring: Client writes, plugin host reads --

    /// Write one block of interleaved float32 audio to the input ring.
    /// @return true if written, false if ring is full (would overwrite unread data).
    bool writeInputBlock(const float* interleavedData);

    /// Read one block from the input ring.
    /// @return true if read, false if ring is empty.
    bool readInputBlock(float* interleavedData);

    // -- Output ring: Plugin host writes, client reads --

    /// Write one block of interleaved float32 audio to the output ring.
    bool writeOutputBlock(const float* interleavedData);

    /// Read one block from the output ring.
    bool readOutputBlock(float* interleavedData);

    // -- Blocking waits (for use in processing loops) --

    /// Wait until input data is available or timeout.
    bool waitForInput(std::chrono::milliseconds timeout);

    /// Wait until output data is available or timeout.
    bool waitForOutput(std::chrono::milliseconds timeout);

    // -- Accessors --

    /// Read-only access to the header.
    const AudioSegmentHeader& header() const;

    /// Mutable access to the header (for setting latency, transport, etc.)
    AudioSegmentHeader& headerMut();

    /// Size of one block in bytes: blockSize * numChannels * sizeof(float)
    uint32_t blockSizeBytes() const;

    /// Name of the shared memory segment.
    const std::string& name() const;

    // === Extension points for future sidechain/send access ===
    // float* sidechainInputBuffer(uint32_t index, uint64_t blockPos);
    // float* sendOutputBuffer(uint32_t index, uint64_t blockPos);

private:
    SharedAudioRing() = default;

    std::string m_name;
#ifdef _WIN32
    std::unique_ptr<boost::interprocess::windows_shared_memory> m_shm;
#else
    std::unique_ptr<boost::interprocess::shared_memory_object> m_shm;
#endif
    std::unique_ptr<boost::interprocess::mapped_region> m_region;

    // Cached pointers into the mapped region
    AudioSegmentHeader* m_header = nullptr;
    float* m_inputRing = nullptr;   // ringBlocks × blockSize × numChannels floats
    float* m_outputRing = nullptr;  // ringBlocks × blockSize × numChannels floats

    /// Initialize cached pointers after mapping.
    void initPointers();

    /// Get pointer to a specific block in a ring.
    float* blockPtr(float* ringBase, uint64_t blockIndex) const;
};

} // namespace rps::audio
