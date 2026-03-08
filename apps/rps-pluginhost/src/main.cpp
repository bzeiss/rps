/// rps-pluginhost — unified plugin host for all formats (VST3, CLAP, …).
/// A single binary that detects the plugin format from the --format or
/// --plugin-path arguments and creates the appropriate host.

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <dbghelp.h>
#include <imm.h>
#include <cstdio>
#include <csignal>
#include <spdlog/spdlog.h>

#pragma comment(lib, "dbghelp.lib")

static LONG WINAPI crashHandler(EXCEPTION_POINTERS* exInfo) {
    DWORD code = exInfo->ExceptionRecord->ExceptionCode;
    void* addr = exInfo->ExceptionRecord->ExceptionAddress;

    HMODULE hModule = nullptr;
    char moduleName[MAX_PATH] = "unknown";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(addr), &hModule)) {
        GetModuleFileNameA(hModule, moduleName, sizeof(moduleName));
    }

    spdlog::error("*** UNHANDLED EXCEPTION: code=0x{:08X} addr={:p} module={} ***",
                  code, addr, moduleName);

    void* stack[30];
    USHORT frames = CaptureStackBackTrace(0, 30, stack, nullptr);
    spdlog::error("Stack trace ({} frames):", frames);
    for (USHORT i = 0; i < frames; i++) {
        HMODULE frameModule = nullptr;
        char frameModName[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               static_cast<LPCSTR>(stack[i]), &frameModule)) {
            GetModuleFileNameA(frameModule, frameModName, sizeof(frameModName));
            char* base = strrchr(frameModName, '\\');
            if (base) memmove(frameModName, base + 1, strlen(base + 1) + 1);
        }
        DWORD_PTR offset = frameModule ?
            (reinterpret_cast<DWORD_PTR>(stack[i]) - reinterpret_cast<DWORD_PTR>(frameModule)) : 0;
        spdlog::error("  [{}] {:p} {}+0x{:X}", i, stack[i], frameModName, offset);
    }

    spdlog::default_logger()->flush();
    fprintf(stderr, "*** UNHANDLED EXCEPTION: code=0x%08lX addr=%p module=%s ***\n",
            code, addr, moduleName);
    fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

#include <rps/gui/GuiWorkerMain.hpp>

// Format-specific hosts
#include "../../rps-pluginhost-vst3/src/Vst3GuiHost.hpp"
#include "../../rps-pluginhost-clap/src/ClapGuiHost.hpp"

#include <algorithm>
#include <string>

/// Check if a flag is present in argv.
static bool hasFlag(int argc, char* argv[], const std::string& flag) {
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == flag) return true;
    }
    return false;
}

/// Detect format from --format and/or --plugin-path arguments.
static std::string detectFormat(int argc, char* argv[]) {
    // First check --format
    for (int i = 1; i < argc - 1; ++i) {
        if (std::string(argv[i]) == "--format") {
            return argv[i + 1];
        }
    }

    // Fall back to file extension from --plugin-path
    for (int i = 1; i < argc - 1; ++i) {
        if (std::string(argv[i]) == "--plugin-path") {
            std::string path = argv[i + 1];
            if (path.size() >= 5 && path.substr(path.size() - 5) == ".vst3") return "vst3";
            if (path.size() >= 5 && path.substr(path.size() - 5) == ".clap") return "clap";
        }
    }

    return "";
}

int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetUnhandledExceptionFilter(crashHandler);
    ImmDisableIME(static_cast<DWORD>(-1));
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
#endif

    auto format = detectFormat(argc, argv);

    // For --help, create any host so GuiWorkerMain can print its help text
    if (hasFlag(argc, argv, "--help") || hasFlag(argc, argv, "-h")) {
        auto host = std::make_unique<rps::scanner::ClapGuiHost>();
        auto result = rps::gui::GuiWorkerMain::run(argc, argv, std::move(host));
#ifdef _WIN32
        CoUninitialize();
#endif
        return result;
    }

    std::unique_ptr<rps::gui::IPluginGuiHost> host;
    if (format == "vst3") {
        host = std::make_unique<rps::scanner::Vst3GuiHost>();
    } else if (format == "clap") {
        host = std::make_unique<rps::scanner::ClapGuiHost>();
    } else {
        fprintf(stderr, "Error: Unknown or missing plugin format '%s'.\n"
                        "Use --format vst3|clap or provide a --plugin-path with a recognized extension.\n",
                format.c_str());
#ifdef _WIN32
        CoUninitialize();
#endif
        return 1;
    }

    auto result = rps::gui::GuiWorkerMain::run(argc, argv, std::move(host));

#ifdef _WIN32
    CoUninitialize();
#endif
    return result;
}
