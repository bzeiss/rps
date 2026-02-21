#pragma once

#include <vector>
#include <string>
#include <boost/filesystem.hpp>

namespace rps::core {

class DefaultPaths {
public:
    // Returns the standard plugin directories for the current OS platform
    static std::vector<boost::filesystem::path> getPluginDirectories();
};

} // namespace rps::core
