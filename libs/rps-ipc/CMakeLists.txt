add_library(rps-ipc STATIC 
    src/Messages.cpp
    src/Connection.cpp
)
target_include_directories(rps-ipc PUBLIC include)
target_link_libraries(rps-ipc PUBLIC Boost::json Boost::interprocess Boost::date_time PRIVATE rps_warnings)

# Boost.Interprocess pulls in Boost.WinAPI which uses nested anonymous types.
# Suppress for all consumers since this is a PUBLIC dependency.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(rps-ipc PUBLIC -Wno-nested-anon-types)
endif()



