add_library(rps-coordinator STATIC
    src/ChannelFormat.cpp
    src/GraphNode.cpp
    src/Graph.cpp
    src/LatencyCalculator.cpp
    src/GraphSerializer.cpp
    src/AudioBuffer.cpp
    src/GraphExecutor.cpp
    src/GraphWorkerMain.cpp
    src/GraphSlicer.cpp
    src/Coordinator.cpp
    src/ProcessSlice.cpp
)
target_include_directories(rps-coordinator PUBLIC include)
find_package(spdlog REQUIRED)
target_link_libraries(rps-coordinator
    PUBLIC
        Boost::json
        rps-audio
        rps-gui
        rps-core
    PRIVATE
        rps_warnings
        spdlog::spdlog
        Boost::program_options
        Boost::filesystem
        Boost::uuid
)

# ---------------------------------------------------------------------------
# Unit tests
# ---------------------------------------------------------------------------
option(RPS_BUILD_COORDINATOR_TESTS "Build rps-coordinator unit tests" ON)
if(RPS_BUILD_COORDINATOR_TESTS)
    add_executable(rps-coordinator-tests
        tests/ChannelFormatTest.cpp
        tests/GraphTest.cpp
        tests/LatencyTest.cpp
        tests/GraphSerializerTest.cpp
        tests/AudioBufferTest.cpp
        tests/GraphExecutorTest.cpp
        tests/CpuTopologyTest.cpp
        tests/GraphSlicerTest.cpp
        tests/CoordinatorTest.cpp
        tests/TestMain.cpp
    )
    target_link_libraries(rps-coordinator-tests
        PRIVATE
            rps-coordinator
            rps_warnings
            spdlog::spdlog
    )
    target_include_directories(rps-coordinator-tests PRIVATE include)
endif()
