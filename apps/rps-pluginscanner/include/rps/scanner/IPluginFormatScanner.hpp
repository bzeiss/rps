#pragma once

#include <rps/ipc/Messages.hpp>
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <boost/filesystem.hpp>

namespace rps::scanner {

using ProgressCallback = std::function<void(int percentage, const std::string& status)>;

class IPluginFormatScanner {
public:
    virtual ~IPluginFormatScanner() = default;

    // The name of the format this scanner handles (must match FormatTraits name, e.g. "vst3", "clap")
    virtual std::string getFormatName() const = 0;

    // Checks if this scanner can handle the given file based on extension/metadata
    virtual bool canHandle(const boost::filesystem::path& pluginPath) const = 0;

    // Performs the actual scan. Calls progressCb to report progress back to the orchestrator.
    // Returns the ScanResult if successful.
    // Throws std::runtime_error on fatal failure.
    virtual rps::ipc::ScanResult scan(const boost::filesystem::path& pluginPath, ProgressCallback progressCb) = 0;
};

// Factory to get the right scanner for a given file
class ScannerFactory {
public:
    static std::vector<std::unique_ptr<IPluginFormatScanner>> createAllScanners();
};

} // namespace rps::scanner
