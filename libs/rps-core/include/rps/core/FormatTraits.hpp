#pragma once

#include <string>
#include <vector>
#include <memory>
#include <boost/filesystem.hpp>

namespace rps::core {

enum class PluginFormat {
    VST2,
    VST3,
    CLAP,
    AAX,
    AU,
    LV2,
    Unknown
};

class IFormatTraits {
public:
    virtual ~IFormatTraits() = default;

    virtual PluginFormat getFormat() const = 0;
    virtual std::string getName() const = 0;
    virtual std::string getExtension() const = 0;
    
    // Returns OS-specific default installation paths
    virtual std::vector<boost::filesystem::path> getDefaultPaths() const = 0;
    
    // Determines if a path represents a valid plugin of this format
    // On macOS, plugins are often directories (.vst3, .component). On Windows/Linux they are usually files.
    virtual bool isPluginPath(const boost::filesystem::path& path) const = 0;
    
    // Whether this format's plugin "file" is actually a directory bundle (e.g. macOS .component)
    // Helps the discovery scanner know when to stop recursing
    virtual bool isBundleDirectory() const = 0;
};

class FormatRegistry {
public:
    FormatRegistry();

    // Get all supported traits
    const std::vector<std::unique_ptr<IFormatTraits>>& getAllTraits() const;

    // Get specific traits by enum
    const IFormatTraits* getTraits(PluginFormat format) const;

    // Get specific traits by short string name (e.g. "vst3", "clap")
    const IFormatTraits* getTraits(const std::string& formatName) const;

    // Parse a comma-separated list of formats (e.g. "vst3,clap")
    std::vector<const IFormatTraits*> parseFormats(const std::string& formatList) const;

private:
    std::vector<std::unique_ptr<IFormatTraits>> m_traits;
};

} // namespace rps::core
