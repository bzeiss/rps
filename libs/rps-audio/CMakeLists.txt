add_library(rps-audio STATIC
    src/SharedAudioRing.cpp
    src/Sdl3AudioDevice.cpp
    src/AudioDeviceFactory.cpp
)
target_include_directories(rps-audio PUBLIC include)
find_package(SDL3 REQUIRED)
find_package(spdlog REQUIRED)
target_link_libraries(rps-audio
    PUBLIC
        Boost::interprocess
        SDL3::SDL3
    PRIVATE
        rps_warnings
        spdlog::spdlog
)
