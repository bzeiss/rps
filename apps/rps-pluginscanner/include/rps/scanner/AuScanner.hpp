#pragma once

#include <rps/scanner/IPluginFormatScanner.hpp>
#include <string>

namespace rps::scanner {

class AuScanner : public IPluginFormatScanner {
public:
    std::string getFormatName() const override { return "au"; }
    bool canHandle(const boost::filesystem::path& pluginPath) const override;
    rps::ipc::ScanResult scan(const boost::filesystem::path& pluginPath, ProgressCallback progressCb) override;
};

} // namespace rps::scanner
