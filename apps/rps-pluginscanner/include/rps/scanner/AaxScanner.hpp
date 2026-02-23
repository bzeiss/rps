#pragma once

#include <rps/scanner/IPluginFormatScanner.hpp>
#include <string>
#include <vector>
#include <map>

namespace rps::scanner {

class AaxScanner : public IPluginFormatScanner {
public:
    std::string getFormatName() const override { return "aax"; }
    bool canHandle(const boost::filesystem::path& pluginPath) const override;
    rps::ipc::ScanResult scan(const boost::filesystem::path& pluginPath, ProgressCallback progressCb) override;

private:
    // Represents a single plugin variant within a .aaxplugin cache file
    struct AaxPluginVariant {
        int index = 0;
        std::string name;
        std::string piName;
        std::string mfgName;
        std::string manufacturerId;
        int64_t manufacturerIdNum = 0;
        std::string productId;
        int64_t productIdNum = 0;
        std::string pluginId;
        int64_t pluginIdNum = 0;
        std::string effectId;
        int pluginType = 0;     // 1=AudioSuite, 3=Native, 8=DSP
        int numInputs = 0;
        int numOutputs = 0;
        int stemFormatInput = 0;
        int stemFormatOutput = 0;
        int stemFormatSidechain = 0;
    };

    // Search Pro Tools cache directories for the .plugincache.txt matching a plugin
    boost::filesystem::path findCacheFile(const boost::filesystem::path& pluginPath) const;

    // Get all Pro Tools cache search directories
    std::vector<boost::filesystem::path> getCacheSearchPaths() const;

    // Parse a .plugincache.txt file into plugin variants
    std::vector<AaxPluginVariant> parseCacheFile(const boost::filesystem::path& cacheFile) const;

    // Pick the best variant (prefer Native stereo, then AudioSuite)
    const AaxPluginVariant* pickBestVariant(const std::vector<AaxPluginVariant>& variants) const;

    // Parse "FourCC (12345)" into (string, int64_t)
    static std::pair<std::string, int64_t> parseFourCCField(const std::string& value);
};

} // namespace rps::scanner
