# ---------------------------------------------------------------------------
# rps-proto: Shared protobuf message library
# Generates C++ code from rps.proto and host.proto.
# Linked by rps-server, rps-ipc, rps-gui, and rps-pluginhost.
# ---------------------------------------------------------------------------

# --- Cross-platform protobuf resolution ---
find_package(protobuf CONFIG QUIET)

if(protobuf_FOUND)
    set(RPS_PROTO_LIB protobuf::libprotobuf)
    if(TARGET protobuf::protoc)
        set(PROTOC_EXECUTABLE protobuf::protoc)
    else()
        find_program(PROTOC_EXECUTABLE protoc REQUIRED)
    endif()
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(PROTOBUF REQUIRED IMPORTED_TARGET protobuf)
    set(RPS_PROTO_LIB PkgConfig::PROTOBUF)
    find_program(PROTOC_EXECUTABLE protoc REQUIRED)
endif()

set(PROTO_DIR "${CMAKE_SOURCE_DIR}/proto")
set(PROTO_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY ${PROTO_OUT_DIR})

# --- Generate C++ from rps.proto ---
set(RPS_PROTO_SRCS "${PROTO_OUT_DIR}/rps.pb.cc")
set(RPS_PROTO_HDRS "${PROTO_OUT_DIR}/rps.pb.h")

add_custom_command(
    OUTPUT ${RPS_PROTO_SRCS} ${RPS_PROTO_HDRS}
    COMMAND ${PROTOC_EXECUTABLE}
        --cpp_out=${PROTO_OUT_DIR}
        -I "${PROTO_DIR}"
        "${PROTO_DIR}/rps.proto"
    DEPENDS "${PROTO_DIR}/rps.proto"
    COMMENT "Generating protobuf C++ sources from rps.proto"
)

# --- Generate C++ from host.proto ---
set(HOST_PROTO_SRCS "${PROTO_OUT_DIR}/host.pb.cc")
set(HOST_PROTO_HDRS "${PROTO_OUT_DIR}/host.pb.h")

add_custom_command(
    OUTPUT ${HOST_PROTO_SRCS} ${HOST_PROTO_HDRS}
    COMMAND ${PROTOC_EXECUTABLE}
        --cpp_out=${PROTO_OUT_DIR}
        -I "${PROTO_DIR}"
        "${PROTO_DIR}/host.proto"
    DEPENDS "${PROTO_DIR}/host.proto" "${PROTO_DIR}/rps.proto"
    COMMENT "Generating protobuf C++ sources from host.proto"
)

# Suppress warnings on generated code
if(MSVC)
    set_source_files_properties(${RPS_PROTO_SRCS} ${HOST_PROTO_SRCS} PROPERTIES COMPILE_FLAGS "/W0")
else()
    set_source_files_properties(${RPS_PROTO_SRCS} ${HOST_PROTO_SRCS} PROPERTIES COMPILE_FLAGS "-Wno-error -w")
endif()

add_library(rps-proto STATIC
    ${RPS_PROTO_SRCS}
    ${HOST_PROTO_SRCS}
)

target_include_directories(rps-proto PUBLIC ${PROTO_OUT_DIR})
target_link_libraries(rps-proto PUBLIC ${RPS_PROTO_LIB})
