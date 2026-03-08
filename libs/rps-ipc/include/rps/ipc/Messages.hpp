#pragma once

#include <string>
#include <vector>
#include <map>
#include <cstdint>

namespace rps::ipc {

// ---------------------------------------------------------------------------
// Scanner data types (used internally by ProcessPool, DatabaseManager, etc.)
// Serialization is handled by scanner.proto at the IPC boundary.
// ---------------------------------------------------------------------------

struct ParameterInfo {
    uint32_t id;
    std::string name;
    double defaultValue;
};

struct ScanResult {
    std::string name;
    std::string vendor;
    std::string version;
    std::string uid;
    std::string description;
    std::string url;
    std::string category;
    std::string format;      // "vst2", "vst3", "clap", "aax", "au", "lv2"
    std::string scanMethod;  // "moduleinfo.json" or "factory" — how metadata was obtained
    uint32_t numInputs = 0;
    uint32_t numOutputs = 0;
    std::vector<ParameterInfo> parameters;
    std::map<std::string, std::string> extraData; // Format-specific metadata (e.g. AAX IDs)
};

} // namespace rps::ipc
