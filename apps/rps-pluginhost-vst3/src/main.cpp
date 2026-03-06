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

    // Get the module name containing the crash address
    HMODULE hModule = nullptr;
    char moduleName[MAX_PATH] = "unknown";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(addr), &hModule)) {
        GetModuleFileNameA(hModule, moduleName, sizeof(moduleName));
    }

    spdlog::error("*** UNHANDLED EXCEPTION: code=0x{:08X} addr={:p} module={} ***",
                  code, addr, moduleName);

    // Walk the stack (up to 30 frames)
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
            // Strip to basename
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
#include "Vst3GuiHost.hpp"

int main(int argc, char* argv[]) {
#ifdef _WIN32
    // Install process-wide crash handler (catches exceptions on ANY thread)
    SetUnhandledExceptionFilter(crashHandler);
    // Disable IMM (Input Method Manager) for ALL threads.
    // JUCE's WndProc and IMM32.dll enter infinite recursion on WM_IME_* messages.
    // This must happen before any windows are created.
    ImmDisableIME(static_cast<DWORD>(-1));
    // Many VST3 plugins (JUCE-based, etc.) require COM on Windows
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
#endif

    auto host = std::make_unique<rps::scanner::Vst3GuiHost>();
    auto result = rps::gui::GuiWorkerMain::run(argc, argv, std::move(host));

#ifdef _WIN32
    CoUninitialize();
#endif
    return result;
}
