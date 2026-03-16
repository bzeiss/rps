# Custom vcpkg triplet for x64 Windows with static linking, using clang-cl.
# Chainloads the clang-cl toolchain for LLVM compilation tools.

set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE "${CMAKE_CURRENT_LIST_DIR}/../cmake/clang-cl-toolchain.cmake")

# ── Discover MSVC tools and SDK paths for vcpkg port builds ──────────────────
# Ports like OpenSSL use nmake (not CMake), so they need INCLUDE/LIB/PATH
# set explicitly. CMake-based ports get these from the toolchain, but
# nmake-based ports rely on environment variables.

find_program(_TRIPLET_VSWHERE vswhere
    PATHS "$ENV{ProgramFiles\(x86\)}/Microsoft Visual Studio/Installer"
)
if(_TRIPLET_VSWHERE)
    execute_process(
        COMMAND "${_TRIPLET_VSWHERE}" -latest -property installationPath
        OUTPUT_VARIABLE _TRIPLET_VS_PATH
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
endif()

if(_TRIPLET_VS_PATH)
    set(_TRIPLET_MSVC_BASE "${_TRIPLET_VS_PATH}/VC/Tools/MSVC")
    if(EXISTS "${_TRIPLET_MSVC_BASE}")
        file(GLOB _TRIPLET_MSVC_VERS RELATIVE "${_TRIPLET_MSVC_BASE}" "${_TRIPLET_MSVC_BASE}/*")
        list(SORT _TRIPLET_MSVC_VERS COMPARE NATURAL ORDER DESCENDING)
        list(GET _TRIPLET_MSVC_VERS 0 _TRIPLET_MSVC_VER)
        set(_TRIPLET_MSVC_BIN "${_TRIPLET_MSVC_BASE}/${_TRIPLET_MSVC_VER}/bin/Hostx64/x64")
        set(_TRIPLET_MSVC_LIB "${_TRIPLET_MSVC_BASE}/${_TRIPLET_MSVC_VER}/lib/x64")
        set(_TRIPLET_MSVC_INC "${_TRIPLET_MSVC_BASE}/${_TRIPLET_MSVC_VER}/include")
    endif()
endif()

set(_TRIPLET_SDK_ROOT "C:/Program Files (x86)/Windows Kits/10")
if(EXISTS "${_TRIPLET_SDK_ROOT}/Lib")
    file(GLOB _TRIPLET_SDK_VERS RELATIVE "${_TRIPLET_SDK_ROOT}/Lib" "${_TRIPLET_SDK_ROOT}/Lib/*")
    list(SORT _TRIPLET_SDK_VERS COMPARE NATURAL ORDER DESCENDING)
    list(GET _TRIPLET_SDK_VERS 0 _TRIPLET_SDK_VER)
endif()

# PATH — nmake and SDK tools
set(_EXTRA_PATH "")
if(_TRIPLET_MSVC_BIN AND EXISTS "${_TRIPLET_MSVC_BIN}")
    list(APPEND _EXTRA_PATH "${_TRIPLET_MSVC_BIN}")
endif()
if(_TRIPLET_SDK_VER)
    set(_p "${_TRIPLET_SDK_ROOT}/bin/${_TRIPLET_SDK_VER}/x64")
    if(EXISTS "${_p}")
        list(APPEND _EXTRA_PATH "${_p}")
    endif()
endif()
if(_EXTRA_PATH)
    list(JOIN _EXTRA_PATH ";" _EXTRA_PATH_STR)
    set(ENV{PATH} "${_EXTRA_PATH_STR};$ENV{PATH}")
endif()

# LIB — linker search paths for SDK and MSVC runtime libs
set(_EXTRA_LIB "")
foreach(_sub IN ITEMS "um/x64" "ucrt/x64")
    if(_TRIPLET_SDK_VER)
        set(_p "${_TRIPLET_SDK_ROOT}/Lib/${_TRIPLET_SDK_VER}/${_sub}")
        if(EXISTS "${_p}")
            list(APPEND _EXTRA_LIB "${_p}")
        endif()
    endif()
endforeach()
if(_TRIPLET_MSVC_LIB AND EXISTS "${_TRIPLET_MSVC_LIB}")
    list(APPEND _EXTRA_LIB "${_TRIPLET_MSVC_LIB}")
endif()
if(_EXTRA_LIB)
    list(JOIN _EXTRA_LIB ";" _EXTRA_LIB_STR)
    set(ENV{LIB} "${_EXTRA_LIB_STR};$ENV{LIB}")
endif()

# INCLUDE — compiler search paths for SDK and MSVC headers
set(_EXTRA_INC "")
foreach(_sub IN ITEMS "ucrt" "um" "shared" "winrt")
    if(_TRIPLET_SDK_VER)
        set(_p "${_TRIPLET_SDK_ROOT}/Include/${_TRIPLET_SDK_VER}/${_sub}")
        if(EXISTS "${_p}")
            list(APPEND _EXTRA_INC "${_p}")
        endif()
    endif()
endforeach()
if(_TRIPLET_MSVC_INC AND EXISTS "${_TRIPLET_MSVC_INC}")
    list(APPEND _EXTRA_INC "${_TRIPLET_MSVC_INC}")
endif()
if(_EXTRA_INC)
    list(JOIN _EXTRA_INC ";" _EXTRA_INC_STR)
    set(ENV{INCLUDE} "${_EXTRA_INC_STR};$ENV{INCLUDE}")
endif()
