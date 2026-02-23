#pragma once

#include <rps/scanner/IPluginFormatScanner.hpp>
#include <string>

namespace rps::scanner {

class Vst2Scanner : public IPluginFormatScanner {
public:
    std::string getFormatName() const override { return "vst2"; }
    bool canHandle(const boost::filesystem::path& pluginPath) const override;
    rps::ipc::ScanResult scan(const boost::filesystem::path& pluginPath, ProgressCallback progressCb) override;
};

} // namespace rps::scanner
