#include <rps/audio/SharedAudioRing.hpp>

#include <spdlog/spdlog.h>

#include <cstring>
#include <stdexcept>
#include <thread>

namespace bip = boost::interprocess;

namespace rps::audio {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Compute total shared memory segment size.
static size_t computeSegmentSize(uint32_t blockSize, uint32_t numChannels, uint32_t ringBlocks) {
    const size_t headerSize = sizeof(AudioSegmentHeader);
    const size_t blockBytes = static_cast<size_t>(blockSize) * numChannels * sizeof(float);
    const size_t ringSize = static_cast<size_t>(ringBlocks) * blockBytes;
    // Two rings: input + output
    return headerSize + 2 * ringSize;
}

// ---------------------------------------------------------------------------
// SharedAudioRing implementation
// ---------------------------------------------------------------------------

SharedAudioRing::~SharedAudioRing() = default;

std::unique_ptr<SharedAudioRing> SharedAudioRing::create(
    const std::string& name,
    uint32_t sampleRate,
    uint32_t blockSize,
    uint32_t numChannels,
    uint32_t ringBlocks)
{
    const size_t segSize = computeSegmentSize(blockSize, numChannels, ringBlocks);

    spdlog::info("SharedAudioRing::create('{}', sr={}, bs={}, ch={}, rings={}, size={})",
                 name, sampleRate, blockSize, numChannels, ringBlocks, segSize);

    // Remove any stale segment with the same name (POSIX only)
#ifndef _WIN32
    bip::shared_memory_object::remove(name.c_str());
#endif

    auto ring = std::unique_ptr<SharedAudioRing>(new SharedAudioRing());
    ring->m_name = name;

    // Create shared memory
#ifdef _WIN32
    // Windows: pagefile-backed named kernel section (accessible from Python)
    ring->m_shm = std::make_unique<bip::windows_shared_memory>(
        bip::create_only, name.c_str(), bip::read_write,
        static_cast<bip::offset_t>(segSize));
#else
    ring->m_shm = std::make_unique<bip::shared_memory_object>(
        bip::create_only, name.c_str(), bip::read_write);
    ring->m_shm->truncate(static_cast<bip::offset_t>(segSize));
#endif

    // Map the entire segment
    ring->m_region = std::make_unique<bip::mapped_region>(*ring->m_shm, bip::read_write);

    // Zero-initialize
    std::memset(ring->m_region->get_address(), 0, segSize);

    // Placement-construct the header
    ring->m_header = new (ring->m_region->get_address()) AudioSegmentHeader{};
    ring->m_header->version = 1;
    ring->m_header->sampleRate = sampleRate;
    ring->m_header->blockSize = blockSize;
    ring->m_header->numChannels = numChannels;
    ring->m_header->ringBlocks = ringBlocks;

    // Initialize atomic positions
    ring->m_header->inputWritePos.store(0, std::memory_order_relaxed);
    ring->m_header->inputReadPos.store(0, std::memory_order_relaxed);
    ring->m_header->outputWritePos.store(0, std::memory_order_relaxed);
    ring->m_header->outputReadPos.store(0, std::memory_order_relaxed);
    ring->m_header->transportState.store(0, std::memory_order_relaxed);

    ring->initPointers();

    spdlog::info("SharedAudioRing created: header={}B, per-ring={}B",
                 sizeof(AudioSegmentHeader),
                 static_cast<size_t>(ringBlocks) * blockSize * numChannels * sizeof(float));

    return ring;
}

std::unique_ptr<SharedAudioRing> SharedAudioRing::open(const std::string& name) {
    spdlog::info("SharedAudioRing::open('{}')", name);

    auto ring = std::unique_ptr<SharedAudioRing>(new SharedAudioRing());
    ring->m_name = name;

    // Open existing segment
#ifdef _WIN32
    ring->m_shm = std::make_unique<bip::windows_shared_memory>(
        bip::open_only, name.c_str(), bip::read_write);
#else
    ring->m_shm = std::make_unique<bip::shared_memory_object>(
        bip::open_only, name.c_str(), bip::read_write);
#endif

    // Map the entire segment
    ring->m_region = std::make_unique<bip::mapped_region>(*ring->m_shm, bip::read_write);

    // Header is at offset 0 — already constructed by the creator
    ring->m_header = reinterpret_cast<AudioSegmentHeader*>(ring->m_region->get_address());

    if (ring->m_header->version != 1) {
        throw std::runtime_error(
            "SharedAudioRing: unsupported version " + std::to_string(ring->m_header->version));
    }

    ring->initPointers();

    spdlog::info("SharedAudioRing opened: sr={}, bs={}, ch={}, rings={}",
                 ring->m_header->sampleRate, ring->m_header->blockSize,
                 ring->m_header->numChannels, ring->m_header->ringBlocks);

    return ring;
}

void SharedAudioRing::remove(const std::string& name) {
#ifndef _WIN32
    bip::shared_memory_object::remove(name.c_str());
#else
    (void)name; // Windows kernel manages lifetime
#endif
}

void SharedAudioRing::initPointers() {
    auto* base = reinterpret_cast<uint8_t*>(m_region->get_address());
    const size_t headerSize = sizeof(AudioSegmentHeader);
    const size_t ringSize = static_cast<size_t>(m_header->ringBlocks)
                          * m_header->blockSize * m_header->numChannels * sizeof(float);

    m_inputRing = reinterpret_cast<float*>(base + headerSize);
    m_outputRing = reinterpret_cast<float*>(base + headerSize + ringSize);
}

float* SharedAudioRing::blockPtr(float* ringBase, uint64_t blockIndex) const {
    const uint64_t slot = blockIndex % m_header->ringBlocks;
    const size_t blockFloats = static_cast<size_t>(m_header->blockSize) * m_header->numChannels;
    return ringBase + slot * blockFloats;
}

uint32_t SharedAudioRing::blockSizeBytes() const {
    return m_header->blockSize * m_header->numChannels * static_cast<uint32_t>(sizeof(float));
}

const std::string& SharedAudioRing::name() const {
    return m_name;
}

const AudioSegmentHeader& SharedAudioRing::header() const {
    return *m_header;
}

AudioSegmentHeader& SharedAudioRing::headerMut() {
    return *m_header;
}

// ---------------------------------------------------------------------------
// Input ring: client (Python) writes, plugin host reads
// ---------------------------------------------------------------------------

bool SharedAudioRing::writeInputBlock(const float* interleavedData) {
    const uint64_t wp = m_header->inputWritePos.load(std::memory_order_relaxed);
    const uint64_t rp = m_header->inputReadPos.load(std::memory_order_acquire);

    // Full if write is ringBlocks ahead of read
    if (wp - rp >= m_header->ringBlocks) {
        return false;
    }

    float* dst = blockPtr(m_inputRing, wp);
    std::memcpy(dst, interleavedData, blockSizeBytes());

    m_header->inputWritePos.store(wp + 1, std::memory_order_release);
    return true;
}

bool SharedAudioRing::readInputBlock(float* interleavedData) {
    const uint64_t rp = m_header->inputReadPos.load(std::memory_order_relaxed);
    const uint64_t wp = m_header->inputWritePos.load(std::memory_order_acquire);

    // Empty if read has caught up to write
    if (rp >= wp) {
        return false;
    }

    const float* src = blockPtr(m_inputRing, rp);
    std::memcpy(interleavedData, src, blockSizeBytes());

    m_header->inputReadPos.store(rp + 1, std::memory_order_release);
    return true;
}

// ---------------------------------------------------------------------------
// Output ring: plugin host writes, client (Python) reads
// ---------------------------------------------------------------------------

bool SharedAudioRing::writeOutputBlock(const float* interleavedData) {
    const uint64_t wp = m_header->outputWritePos.load(std::memory_order_relaxed);
    const uint64_t rp = m_header->outputReadPos.load(std::memory_order_acquire);

    if (wp - rp >= m_header->ringBlocks) {
        return false;
    }

    float* dst = blockPtr(m_outputRing, wp);
    std::memcpy(dst, interleavedData, blockSizeBytes());

    m_header->outputWritePos.store(wp + 1, std::memory_order_release);
    return true;
}

bool SharedAudioRing::readOutputBlock(float* interleavedData) {
    const uint64_t rp = m_header->outputReadPos.load(std::memory_order_relaxed);
    const uint64_t wp = m_header->outputWritePos.load(std::memory_order_acquire);

    if (rp >= wp) {
        return false;
    }

    const float* src = blockPtr(m_outputRing, rp);
    std::memcpy(interleavedData, src, blockSizeBytes());

    m_header->outputReadPos.store(rp + 1, std::memory_order_release);
    return true;
}

// ---------------------------------------------------------------------------
// Blocking waits — spin-then-yield strategy
// Phase 1: Python is the clock, so spin-wait is acceptable.
// Future DAW: audio device callback replaces this (no waiting needed).
// ---------------------------------------------------------------------------

bool SharedAudioRing::waitForInput(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    int spins = 0;

    while (true) {
        const uint64_t rp = m_header->inputReadPos.load(std::memory_order_relaxed);
        const uint64_t wp = m_header->inputWritePos.load(std::memory_order_acquire);
        if (wp > rp) return true;

        if (std::chrono::steady_clock::now() >= deadline) return false;

        // Spin for a bit, then yield, then sleep briefly
        if (spins < 100) {
            ++spins;
            // Busy spin
        } else if (spins < 200) {
            ++spins;
            std::this_thread::yield();
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
    }
}

bool SharedAudioRing::waitForOutput(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    int spins = 0;

    while (true) {
        const uint64_t rp = m_header->outputReadPos.load(std::memory_order_relaxed);
        const uint64_t wp = m_header->outputWritePos.load(std::memory_order_acquire);
        if (wp > rp) return true;

        if (std::chrono::steady_clock::now() >= deadline) return false;

        if (spins < 100) {
            ++spins;
        } else if (spins < 200) {
            ++spins;
            std::this_thread::yield();
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
    }
}

} // namespace rps::audio
