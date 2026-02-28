#pragma once

#include <string>
#include <vector>
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4100)
#pragma warning(disable: 4244)
#pragma warning(disable: 4245)
#endif
#include <boost/filesystem.hpp>
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#include <rps/core/FormatTraits.hpp>

namespace rps::core {

class PluginDiscovery {
public:
    static std::vector<boost::filesystem::path> findPlugins(
        const std::vector<std::string>& directories,
        const std::vector<const IFormatTraits*>& formats);

private:
    static bool isPluginFile(const boost::filesystem::path& path, const std::vector<const IFormatTraits*>& formats);
    static bool isBundleDirectory(const boost::filesystem::path& path, const std::vector<const IFormatTraits*>& formats);
};

} // namespace rps::core
