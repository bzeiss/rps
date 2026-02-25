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
    find_package(protobuf CONFIG REQUIRED)
    set(RPS_GRPC_LIB gRPC::grpc++)
    set(RPS_PROTO_LIB protobuf::libprotobuf)

    # 2. Find Tools (vcpkg Native Targets)
    set(PROTOC_EXECUTABLE protobuf::protoc)
    set(GRPC_CPP_PLUGIN $<TARGET_FILE:gRPC::grpc_cpp_plugin>)
endif()

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
        ${RPS_GRPC_LIB}
        ${RPS_PROTO_LIB}
        spdlog::spdlog
        Boost::program_options
        Boost::dll
)
