#pragma once

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string>

class WindowsProcessJob {
public:
    WindowsProcessJob() = default;
    ~WindowsProcessJob();

    WindowsProcessJob(const WindowsProcessJob&) = delete;
    WindowsProcessJob& operator=(const WindowsProcessJob&) = delete;

    bool attach(HANDLE processHandle, std::string* err = nullptr);
    void close();

private:
    HANDLE m_job = nullptr;
};

#endif

