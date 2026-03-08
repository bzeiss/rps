add_library(rps-core STATIC 
    src/PluginDiscovery.cpp
    src/FormatTraits.cpp
    src/LoggingInit.cpp
)
target_include_directories(rps-core PUBLIC include)
find_package(spdlog REQUIRED)
target_link_libraries(rps-core PUBLIC Boost::filesystem spdlog::spdlog PRIVATE rps_warnings)
