add_library(rps-core STATIC src/PluginDiscovery.cpp)
target_include_directories(rps-core PUBLIC include)
target_link_libraries(rps-core PUBLIC Boost::filesystem)

