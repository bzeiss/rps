#include <rps/audio/SharedAudioRing.hpp>

#include <spdlog/spdlog.h>

#include <cstring>
#include <stdexcept>
#include <thread>

#ifndef _WIN32
#include <fcntl.h>
#include <time.h>
#endif

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

SharedAudioRing::~SharedAudioRing() {
    closeEvents();
}

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

    // Create named OS events for cross-process signaling
    ring->m_isCreator = true;
    ring->createEvents(name);

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

    // Open named OS events created by the server
    ring->m_isCreator = false;
    ring->openEvents(name);

    spdlog::info("SharedAudioRing opened: sr={}, bs={}, ch={}, rings={}",
                 ring->m_header->sampleRate, ring->m_header->blockSize,
                 ring->m_header->numChannels, ring->m_header->ringBlocks);

    return ring;
}

void SharedAudioRing::remove(const std::string& name) {
#ifdef _WIN32
    (void)name; // Windows kernel manages event/shm lifetime
#else
    bip::shared_memory_object::remove(name.c_str());
    // Also clean up named semaphores
    std::string inputName = "/rps-audio-" + name + "-input";
    std::string outputName = "/rps-audio-" + name + "-output";
    sem_unlink(inputName.c_str());
    sem_unlink(outputName.c_str());
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

    // Signal the consumer that input data is available
#ifdef _WIN32
    if (m_inputEvent) SetEvent(m_inputEvent);
#else
    if (m_inputSem != SEM_FAILED) sem_post(m_inputSem);
#endif

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

    // Signal the consumer that output data is available
#ifdef _WIN32
    if (m_outputEvent) SetEvent(m_outputEvent);
#else
    if (m_outputSem != SEM_FAILED) sem_post(m_outputSem);
#endif

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
// Blocking waits — OS event signaling for ~1-5μs wake-up latency.
// Replaces the previous spin-poll-sleep strategy which suffered from
// Windows timer resolution (sleep_for(10μs) → actual 1-15ms sleep).
// ---------------------------------------------------------------------------

bool SharedAudioRing::waitForInput(std::chrono::milliseconds timeout) {
    // Fast path: data already available (no syscall)
    if (m_header->inputWritePos.load(std::memory_order_acquire)
        > m_header->inputReadPos.load(std::memory_order_relaxed)) {
        return true;
    }

    // Slow path: wait on OS event
#ifdef _WIN32
    if (m_inputEvent) {
        DWORD result = WaitForSingleObject(m_inputEvent, static_cast<DWORD>(timeout.count()));
        if (result == WAIT_OBJECT_0) {
            // Re-check atomic — auto-reset events can fire spuriously if
            // multiple signals collapse into one
            return m_header->inputWritePos.load(std::memory_order_acquire)
                   > m_header->inputReadPos.load(std::memory_order_relaxed);
        }
        return false;
    }
#else
    if (m_inputSem != SEM_FAILED) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += (timeout.count() % 1000) * 1'000'000;
        ts.tv_sec += timeout.count() / 1000 + ts.tv_nsec / 1'000'000'000;
        ts.tv_nsec %= 1'000'000'000;
        if (sem_timedwait(m_inputSem, &ts) == 0) {
            return m_header->inputWritePos.load(std::memory_order_acquire)
                   > m_header->inputReadPos.load(std::memory_order_relaxed);
        }
        return false;
    }
#endif

    // Fallback if events weren't available: spin-yield
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (m_header->inputWritePos.load(std::memory_order_acquire)
            > m_header->inputReadPos.load(std::memory_order_relaxed)) {
            return true;
        }
        std::this_thread::yield();
    }
    return false;
}

bool SharedAudioRing::waitForOutput(std::chrono::milliseconds timeout) {
    // Fast path: data already available (no syscall)
    if (m_header->outputWritePos.load(std::memory_order_acquire)
        > m_header->outputReadPos.load(std::memory_order_relaxed)) {
        return true;
    }

    // Slow path: wait on OS event
#ifdef _WIN32
    if (m_outputEvent) {
        DWORD result = WaitForSingleObject(m_outputEvent, static_cast<DWORD>(timeout.count()));
        if (result == WAIT_OBJECT_0) {
            return m_header->outputWritePos.load(std::memory_order_acquire)
                   > m_header->outputReadPos.load(std::memory_order_relaxed);
        }
        return false;
    }
#else
    if (m_outputSem != SEM_FAILED) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += (timeout.count() % 1000) * 1'000'000;
        ts.tv_sec += timeout.count() / 1000 + ts.tv_nsec / 1'000'000'000;
        ts.tv_nsec %= 1'000'000'000;
        if (sem_timedwait(m_outputSem, &ts) == 0) {
            return m_header->outputWritePos.load(std::memory_order_acquire)
                   > m_header->outputReadPos.load(std::memory_order_relaxed);
        }
        return false;
    }
#endif

    // Fallback
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (m_header->outputWritePos.load(std::memory_order_acquire)
            > m_header->outputReadPos.load(std::memory_order_relaxed)) {
            return true;
        }
        std::this_thread::yield();
    }
    return false;
}

// ---------------------------------------------------------------------------
// OS event lifecycle — named events for cross-process signaling
// ---------------------------------------------------------------------------

void SharedAudioRing::createEvents(const std::string& baseName) {
#ifdef _WIN32
    // Use wide strings for Windows named events.
    // Auto-reset events: WaitForSingleObject automatically resets to non-signaled.
    m_inputEventName = "rps-audio-" + baseName + "-input";
    m_outputEventName = "rps-audio-" + baseName + "-output";

    std::wstring wInput(m_inputEventName.begin(), m_inputEventName.end());
    std::wstring wOutput(m_outputEventName.begin(), m_outputEventName.end());

    m_inputEvent = CreateEventW(nullptr, FALSE /*auto-reset*/, FALSE /*initial*/, wInput.c_str());
    m_outputEvent = CreateEventW(nullptr, FALSE, FALSE, wOutput.c_str());

    if (!m_inputEvent || !m_outputEvent) {
        spdlog::warn("Failed to create audio events (error {}), falling back to polling",
                     GetLastError());
        closeEvents();
    } else {
        spdlog::info("Created audio events: '{}', '{}'", m_inputEventName, m_outputEventName);
    }
#else
    m_inputEventName = "/rps-audio-" + baseName + "-input";
    m_outputEventName = "/rps-audio-" + baseName + "-output";

    // Remove stale semaphores from previous runs
    sem_unlink(m_inputEventName.c_str());
    sem_unlink(m_outputEventName.c_str());

    m_inputSem = sem_open(m_inputEventName.c_str(), O_CREAT | O_EXCL, 0644, 0);
    m_outputSem = sem_open(m_outputEventName.c_str(), O_CREAT | O_EXCL, 0644, 0);

    if (m_inputSem == SEM_FAILED || m_outputSem == SEM_FAILED) {
        spdlog::warn("Failed to create audio semaphores, falling back to polling");
        closeEvents();
    } else {
        spdlog::info("Created audio semaphores: '{}', '{}'", m_inputEventName, m_outputEventName);
    }
#endif
}

void SharedAudioRing::openEvents(const std::string& baseName) {
#ifdef _WIN32
    m_inputEventName = "rps-audio-" + baseName + "-input";
    m_outputEventName = "rps-audio-" + baseName + "-output";

    std::wstring wInput(m_inputEventName.begin(), m_inputEventName.end());
    std::wstring wOutput(m_outputEventName.begin(), m_outputEventName.end());

    m_inputEvent = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, wInput.c_str());
    m_outputEvent = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, wOutput.c_str());

    if (!m_inputEvent || !m_outputEvent) {
        spdlog::warn("Failed to open audio events (error {}), falling back to polling",
                     GetLastError());
        closeEvents();
    } else {
        spdlog::info("Opened audio events: '{}', '{}'", m_inputEventName, m_outputEventName);
    }
#else
    m_inputEventName = "/rps-audio-" + baseName + "-input";
    m_outputEventName = "/rps-audio-" + baseName + "-output";

    m_inputSem = sem_open(m_inputEventName.c_str(), 0);
    m_outputSem = sem_open(m_outputEventName.c_str(), 0);

    if (m_inputSem == SEM_FAILED || m_outputSem == SEM_FAILED) {
        spdlog::warn("Failed to open audio semaphores, falling back to polling");
        closeEvents();
    } else {
        spdlog::info("Opened audio semaphores: '{}', '{}'", m_inputEventName, m_outputEventName);
    }
#endif
}

void SharedAudioRing::closeEvents() {
#ifdef _WIN32
    if (m_inputEvent) {
        CloseHandle(m_inputEvent);
        m_inputEvent = nullptr;
    }
    if (m_outputEvent) {
        CloseHandle(m_outputEvent);
        m_outputEvent = nullptr;
    }
#else
    if (m_inputSem != SEM_FAILED) {
        sem_close(m_inputSem);
        m_inputSem = SEM_FAILED;
    }
    if (m_outputSem != SEM_FAILED) {
        sem_close(m_outputSem);
        m_outputSem = SEM_FAILED;
    }
    // Only unlink if we created them (server side)
    if (m_isCreator) {
        if (!m_inputEventName.empty()) sem_unlink(m_inputEventName.c_str());
        if (!m_outputEventName.empty()) sem_unlink(m_outputEventName.c_str());
    }
#endif
}

} // namespace rps::audio
