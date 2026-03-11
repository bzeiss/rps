add_library(rps-gui STATIC
    src/SdlWindow.cpp
    src/GuiWorkerMain.cpp
)

target_include_directories(rps-gui PUBLIC include)

find_package(SDL3 REQUIRED)
find_package(spdlog REQUIRED)
find_package(imgui CONFIG REQUIRED)

if(UNIX AND NOT APPLE)
    find_package(X11 REQUIRED)
endif()

target_link_libraries(rps-gui
    PUBLIC
        rps-core
        rps-ipc
        rps-audio
        SDL3::SDL3
        imgui::imgui
        Boost::filesystem
        Boost::program_options
    PRIVATE
        rps_warnings
        $<$<PLATFORM_ID:Linux>:X11::X11>
)
