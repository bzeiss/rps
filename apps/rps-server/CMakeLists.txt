# --- Cross-platform gRPC resolution ---
# Protobuf message types come from rps-proto (via rps-ipc).
# Here we only need gRPC for the service stubs.
find_package(gRPC CONFIG QUIET)
find_package(protobuf CONFIG QUIET)

if(gRPC_FOUND AND protobuf_FOUND)
    set(RPS_GRPC_LIB gRPC::grpc++)

    if(TARGET protobuf::protoc)
        set(PROTOC_EXECUTABLE protobuf::protoc)
    else()
        find_program(PROTOC_EXECUTABLE protoc REQUIRED)
    endif()

    if(TARGET gRPC::grpc_cpp_plugin)
        set(GRPC_CPP_PLUGIN $<TARGET_FILE:gRPC::grpc_cpp_plugin>)
    else()
        find_program(GRPC_CPP_PLUGIN grpc_cpp_plugin REQUIRED)
    endif()
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GRPCPP REQUIRED IMPORTED_TARGET grpc++)
    set(RPS_GRPC_LIB PkgConfig::GRPCPP)

    find_program(PROTOC_EXECUTABLE protoc REQUIRED)
    find_program(GRPC_CPP_PLUGIN grpc_cpp_plugin REQUIRED)
endif()

find_package(spdlog REQUIRED)

# Generate only gRPC service stubs (protobuf messages come from rps-proto)
set(PROTO_FILE "${CMAKE_SOURCE_DIR}/proto/rps.proto")
set(GRPC_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY ${GRPC_OUT_DIR})

set(GRPC_SRCS "${GRPC_OUT_DIR}/rps.grpc.pb.cc")
set(GRPC_HDRS "${GRPC_OUT_DIR}/rps.grpc.pb.h")

add_custom_command(
    OUTPUT ${GRPC_SRCS} ${GRPC_HDRS}
    COMMAND ${PROTOC_EXECUTABLE}
        --grpc_out=${GRPC_OUT_DIR}
        --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}
        -I "${CMAKE_SOURCE_DIR}/proto"
        "${PROTO_FILE}"
    DEPENDS "${PROTO_FILE}"
    COMMENT "Generating gRPC C++ service stubs from rps.proto"
)

# Suppress warnings on machine-generated code
if(MSVC)
    set_source_files_properties(${GRPC_SRCS} PROPERTIES COMPILE_FLAGS "/W0")
else()
    set_source_files_properties(${GRPC_SRCS} PROPERTIES COMPILE_FLAGS "-Wno-error -w")
endif()

add_executable(rps-server
    src/main.cpp
    src/RpsServiceImpl.cpp
    src/GrpcScanObserver.cpp
    src/ProtobufCompat.cpp
    src/GuiSessionManager.cpp
    ${GRPC_SRCS}
)

target_include_directories(rps-server
    PRIVATE
        include
        ${GRPC_OUT_DIR}
)

target_link_libraries(rps-server
    PRIVATE
        rps-engine
        rps-coordinator
        rps-ipc
        rps-audio
        rps_warnings
        ${RPS_GRPC_LIB}
        spdlog::spdlog
        SQLite::SQLite3
        Boost::program_options
        Boost::filesystem
        Boost::dll
)

# Ensure the scanner and plugin host are built before we try to copy them
add_dependencies(rps-server rps-pluginscanner rps-pluginhost)

# POST_BUILD: copy on rebuild
add_custom_command(TARGET rps-server POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:rps-pluginscanner>
        "$<TARGET_FILE_DIR:rps-server>/$<TARGET_FILE_NAME:rps-pluginscanner>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:rps-pluginhost>
        "$<TARGET_FILE_DIR:rps-server>/$<TARGET_FILE_NAME:rps-pluginhost>"
    COMMAND ${CMAKE_COMMAND}
        -DSRC=$<TARGET_FILE:rps-server>
        -DDST=${CMAKE_SOURCE_DIR}/examples/python/$<TARGET_FILE_NAME:rps-server>
        -P ${CMAKE_SOURCE_DIR}/cmake/try_copy.cmake
    COMMAND ${CMAKE_COMMAND}
        -DSRC=$<TARGET_FILE:rps-server>
        -DDST=${CMAKE_SOURCE_DIR}/examples/java/$<TARGET_FILE_NAME:rps-server>
        -P ${CMAKE_SOURCE_DIR}/cmake/try_copy.cmake
    COMMENT "Copying server and deps to output directories"
    COMMAND_EXPAND_LISTS
)

# Always-run: ensure copies exist even when nothing was rebuilt
add_custom_target(copy-server-deps ALL
    COMMAND ${CMAKE_COMMAND}
        -DSRC=$<TARGET_FILE:rps-server>
        -DDST=${CMAKE_SOURCE_DIR}/examples/python/$<TARGET_FILE_NAME:rps-server>
        -P ${CMAKE_SOURCE_DIR}/cmake/try_copy.cmake
    COMMAND ${CMAKE_COMMAND}
        -DSRC=$<TARGET_FILE:rps-server>
        -DDST=${CMAKE_SOURCE_DIR}/examples/java/$<TARGET_FILE_NAME:rps-server>
        -P ${CMAKE_SOURCE_DIR}/cmake/try_copy.cmake
    DEPENDS rps-server
    COMMENT "Ensuring server binaries are present in example directories"
)
