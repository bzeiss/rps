# Use pkg-config for gRPC and protobuf — the CMake CONFIG packages on MSYS2
# have a broken protobuf-targets.cmake that references missing UPB targets.
find_package(PkgConfig REQUIRED)
pkg_check_modules(GRPCPP REQUIRED IMPORTED_TARGET grpc++)
pkg_check_modules(PROTOBUF REQUIRED IMPORTED_TARGET protobuf)
find_package(spdlog REQUIRED)

# Proto codegen
set(PROTO_FILE "${CMAKE_SOURCE_DIR}/proto/rps.proto")
set(PROTO_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY ${PROTO_OUT_DIR})

# Find protoc and grpc_cpp_plugin
find_program(PROTOC_EXECUTABLE protoc REQUIRED)
find_program(GRPC_CPP_PLUGIN grpc_cpp_plugin REQUIRED)

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
set_source_files_properties(${PROTO_SRCS} ${GRPC_SRCS} PROPERTIES
    COMPILE_FLAGS "-Wno-error -w"
)

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
        PkgConfig::GRPCPP
        PkgConfig::PROTOBUF
        spdlog::spdlog
        Boost::program_options
        Boost::filesystem
        Boost::dll
)
