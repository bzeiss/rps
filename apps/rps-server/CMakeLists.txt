# --- Cross-platform gRPC / Protobuf resolution ---
if(MINGW OR MSYS)
    # 1. Find Libraries (MSYS2 Workaround)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GRPCPP REQUIRED IMPORTED_TARGET grpc++)
    pkg_check_modules(PROTOBUF REQUIRED IMPORTED_TARGET protobuf)
    set(RPS_GRPC_LIB PkgConfig::GRPCPP)
    set(RPS_PROTO_LIB PkgConfig::PROTOBUF)

    # 2. Find Tools (MSYS2 Standard Path)
    find_program(PROTOC_EXECUTABLE protoc REQUIRED)
    find_program(GRPC_CPP_PLUGIN grpc_cpp_plugin REQUIRED)
else()
    # 1. Find Libraries (vcpkg / MSVC / Native CMake)
    find_package(gRPC CONFIG REQUIRED)
    set(RPS_GRPC_LIB gRPC::grpc++)

    # Try protobuf config (vcpkg) first, then fallback to FindProtobuf module
    find_package(protobuf CONFIG QUIET)
    if(NOT protobuf_FOUND)
        find_package(Protobuf REQUIRED)
    endif()
    set(RPS_PROTO_LIB protobuf::libprotobuf)

    # 2. Find Tools
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
endif()

find_package(spdlog REQUIRED)

# Proto codegen
set(PROTO_FILE "${CMAKE_SOURCE_DIR}/proto/rps.proto")
set(PROTO_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY ${PROTO_OUT_DIR})

# Generate protobuf and gRPC C++ sources
set(PROTO_SRCS "${PROTO_OUT_DIR}/rps.pb.cc")
set(PROTO_HDRS "${PROTO_OUT_DIR}/rps.pb.h")
set(GRPC_SRCS "${PROTO_OUT_DIR}/rps.grpc.pb.cc")
set(GRPC_HDRS "${PROTO_OUT_DIR}/rps.grpc.pb.h")

add_custom_command(
    OUTPUT ${PROTO_SRCS} ${PROTO_HDRS} ${GRPC_SRCS} ${GRPC_HDRS}
    COMMAND ${PROTOC_EXECUTABLE}
        --cpp_out=${PROTO_OUT_DIR}
        --grpc_out=${PROTO_OUT_DIR}
        --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}
        -I "${CMAKE_SOURCE_DIR}/proto"
        "${PROTO_FILE}"
    DEPENDS "${PROTO_FILE}"
    COMMENT "Generating gRPC/Protobuf C++ sources from rps.proto"
)

# Generated protobuf/gRPC sources use abseil macros that trigger Clang warnings
# (_Nullable, _Nonnull extensions). Disable warnings on machine-generated code.
if(MSVC)
    set_source_files_properties(${PROTO_SRCS} ${GRPC_SRCS} PROPERTIES COMPILE_FLAGS "/W0")
else()
    set_source_files_properties(${PROTO_SRCS} ${GRPC_SRCS} PROPERTIES COMPILE_FLAGS "-Wno-error -w")
endif()

add_executable(rps-server
    src/main.cpp
    src/RpsServiceImpl.cpp
    src/GrpcScanObserver.cpp
    ${PROTO_SRCS}
    ${GRPC_SRCS}
)

target_include_directories(rps-server
    PRIVATE
        include
        ${PROTO_OUT_DIR}
)

target_link_libraries(rps-server
    PRIVATE
        rps-engine
        rps_warnings
        ${RPS_GRPC_LIB}
        ${RPS_PROTO_LIB}
        spdlog::spdlog
        Boost::program_options
        Boost::dll
)

# Ensure the scanner is built before we try to copy it
add_dependencies(rps-server rps-pluginscanner)

# After build, copy rps-server and rps-pluginscanner to necessary directories
add_custom_command(
    TARGET rps-server POST_BUILD
    # Copy scanner next to server
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:rps-pluginscanner>
        "$<TARGET_FILE_DIR:rps-server>/$<TARGET_FILE_NAME:rps-pluginscanner>"
    # Copy server to Python and Java example directories
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:rps-server>
        "${CMAKE_SOURCE_DIR}/examples/python/$<TARGET_FILE_NAME:rps-server>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:rps-server>
        "${CMAKE_SOURCE_DIR}/examples/java/$<TARGET_FILE_NAME:rps-server>"
    COMMENT "Copying binaries to server and example directories"
)
