# try_copy.cmake — Best-effort file copy that runs every build.
# Uses copy_if_different for speed; logs a warning instead of failing
# if the destination is locked (e.g. a running process holds it open).
#
# Usage: cmake -DSRC=<source> -DDST=<destination> -P try_copy.cmake

if(NOT DEFINED SRC OR NOT DEFINED DST)
    message(FATAL_ERROR "try_copy.cmake: SRC and DST must be defined")
endif()

# Force the copy if the destination doesn't exist, otherwise use copy_if_different
if(NOT EXISTS "${DST}")
    set(_CMD copy)
else()
    set(_CMD copy_if_different)
endif()

execute_process(
    COMMAND ${CMAKE_COMMAND} -E ${_CMD} "${SRC}" "${DST}"
    RESULT_VARIABLE result
)

if(NOT result EQUAL 0)
    message(WARNING "try_copy: could not copy ${SRC} -> ${DST} (file may be locked)")
endif()
