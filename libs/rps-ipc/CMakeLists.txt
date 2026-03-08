add_library(rps-ipc STATIC 
    src/Messages.cpp
    src/Connection.cpp
    src/StdioPipeConnection.cpp
)
target_include_directories(rps-ipc PUBLIC include)
target_link_libraries(rps-ipc PUBLIC rps-proto Boost::json Boost::interprocess Boost::date_time PRIVATE rps_warnings)



