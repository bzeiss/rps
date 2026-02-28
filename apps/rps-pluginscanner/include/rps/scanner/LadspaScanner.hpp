#pragma once

#include <rps/scanner/IPluginFormatScanner.hpp>

namespace rps::scanner {

class LadspaScanner : public IPluginFormatScanner {
public:
    std::string getFormatName() const override { return "ladspa"; }
    bool canHandle(const boost::filesystem::path& pluginPath) const override;
    rps::ipc::ScanResult scan(const boost::filesystem::path& pluginPath, ProgressCallback progressCb) override;
};

} // namespace rps::scanner
