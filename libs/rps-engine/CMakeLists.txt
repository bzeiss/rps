add_library(rps-engine STATIC
    src/ScanEngine.cpp
    src/ProcessPool.cpp
    src/ConsoleScanObserver.cpp
    src/db/DatabaseManager.cpp
)

target_include_directories(rps-engine
    PUBLIC include
    PRIVATE ${BOOST_SOURCE_DIR}/libs/crc/include
)

target_link_libraries(rps-engine
    PUBLIC
        rps-core
        rps-ipc
        Boost::json
        Boost::filesystem
        SQLite::SQLite3
    PRIVATE
        rps_warnings
        Boost::process
        Boost::interprocess
        Boost::uuid
        Boost::dll
)

if(WIN32)
    target_link_libraries(rps-engine PUBLIC ws2_32)
endif()
