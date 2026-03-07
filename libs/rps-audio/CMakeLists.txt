add_library(rps-audio STATIC
    src/SharedAudioRing.cpp
)
target_include_directories(rps-audio PUBLIC include)
find_package(spdlog REQUIRED)
target_link_libraries(rps-audio PUBLIC Boost::interprocess PRIVATE rps_warnings spdlog::spdlog)
