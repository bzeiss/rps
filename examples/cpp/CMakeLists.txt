# C++ gRPC example client for RPS
# Reuses the same pkg-config / proto codegen approach as rps-server.

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

# Proto codegen — generate into our own build directory
set(PROTO_FILE "${CMAKE_SOURCE_DIR}/proto/rps.proto")
set(PROTO_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY ${PROTO_OUT_DIR})

find_program(PROTOC_EXECUTABLE protoc REQUIRED)
find_program(GRPC_CPP_PLUGIN grpc_cpp_plugin REQUIRED)

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
    COMMENT "Generating gRPC/Protobuf C++ sources for rps-example-client"
)

# Suppress warnings on generated code
if(MSVC)
    set_source_files_properties(${PROTO_SRCS} ${GRPC_SRCS} PROPERTIES
        COMPILE_FLAGS "/W0"
    )
else()
    set_source_files_properties(${PROTO_SRCS} ${GRPC_SRCS} PROPERTIES
        COMPILE_FLAGS "-w"
    )
endif()

add_executable(rps-example-client
    src/main.cpp
    ${PROTO_SRCS}
    ${GRPC_SRCS}
)

target_include_directories(rps-example-client
    PRIVATE
        ${PROTO_OUT_DIR}
)

target_link_libraries(rps-example-client
    PRIVATE
        rps_warnings
        ${RPS_GRPC_LIB}
        ${RPS_PROTO_LIB}
        Boost::program_options
        Boost::filesystem
)

# Ensure rps-server binary is next to the client for easy execution
add_custom_command(
    TARGET rps-example-client POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:rps-server>
        "$<TARGET_FILE_DIR:rps-example-client>/$<TARGET_FILE_NAME:rps-server>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:rps-pluginscanner>
        "$<TARGET_FILE_DIR:rps-example-client>/$<TARGET_FILE_NAME:rps-pluginscanner>"
    COMMENT "Copying rps-server and rps-pluginscanner to C++ example output directory"
)
