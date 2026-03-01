# C++ gRPC example client for RPS.

# --- Cross-platform gRPC / Protobuf resolution ---
# Prefer CMake config packages (works with vcpkg on all platforms, including MinGW).
# Fall back to pkg-config only when config packages are unavailable.
find_package(gRPC CONFIG QUIET)
find_package(protobuf CONFIG QUIET)

if(gRPC_FOUND AND protobuf_FOUND)
    set(RPS_GRPC_LIB gRPC::grpc++)
    set(RPS_PROTO_LIB protobuf::libprotobuf)
    if(TARGET protobuf::libprotobuf-lite)
        list(APPEND RPS_PROTO_LIB protobuf::libprotobuf-lite)
    endif()

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
    pkg_check_modules(PROTOBUF REQUIRED IMPORTED_TARGET protobuf)
    set(RPS_GRPC_LIB PkgConfig::GRPCPP)
    set(RPS_PROTO_LIB PkgConfig::PROTOBUF)

    find_program(PROTOC_EXECUTABLE protoc REQUIRED)
    find_program(GRPC_CPP_PLUGIN grpc_cpp_plugin REQUIRED)
endif()

# Proto codegen — generate into our own build directory
set(PROTO_FILE "${CMAKE_SOURCE_DIR}/proto/rps.proto")
set(PROTO_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY ${PROTO_OUT_DIR})

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
    src/ProtobufCompat.cpp
    src/WindowsProcessJob.cpp
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

add_custom_target(rps-example-client-deps ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:rps-server>
        "$<TARGET_FILE_DIR:rps-example-client>/$<TARGET_FILE_NAME:rps-server>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:rps-pluginscanner>
        "$<TARGET_FILE_DIR:rps-example-client>/$<TARGET_FILE_NAME:rps-pluginscanner>"
    DEPENDS rps-server rps-pluginscanner
    COMMENT "Ensuring server binaries are present for C++ example"
)
add_dependencies(rps-example-client rps-example-client-deps)
