#ifdef _WIN32

#include "WindowsProcessJob.hpp"

#include <cstdlib>
#include <iostream>

namespace {
bool processDebugEnabled() {
    const char* v = std::getenv("RPS_DEBUG_PROCESS_LIFECYCLE");
    return v && std::string(v) == "1";
}
}

WindowsProcessJob::~WindowsProcessJob() {
    close();
}

bool WindowsProcessJob::attach(HANDLE processHandle, std::string* err) {
    close();

    m_job = CreateJobObjectA(nullptr, nullptr);
    if (!m_job) {
        if (err) *err = "failed to create Windows Job Object (GetLastError=" + std::to_string(GetLastError()) + ")";
        return false;
    }

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(m_job, JobObjectExtendedLimitInformation, &info, sizeof(info))) {
        if (err) *err = "failed to configure Windows Job Object (GetLastError=" + std::to_string(GetLastError()) + ")";
        close();
        return false;
    }

    if (!AssignProcessToJobObject(m_job, processHandle)) {
        if (err) *err = "failed to assign process to Windows Job Object (GetLastError=" + std::to_string(GetLastError()) + ")";
        close();
        return false;
    }

    if (processDebugEnabled()) {
        std::cerr << "[rps] attached Windows Job Object\n";
    }
    return true;
}

void WindowsProcessJob::close() {
    if (m_job) {
        CloseHandle(m_job);
        m_job = nullptr;
        if (processDebugEnabled()) {
            std::cerr << "[rps] closed Windows Job Object\n";
        }
    }
}

#endif

