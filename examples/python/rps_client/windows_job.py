"""Windows Job Object helper for deterministic child-process cleanup."""

from __future__ import annotations

import ctypes
from ctypes import wintypes


_kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)


JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x00002000
JOB_OBJECT_EXTENDED_LIMIT_INFORMATION = 9

PROCESS_TERMINATE = 0x0001
PROCESS_SET_QUOTA = 0x0100


class IO_COUNTERS(ctypes.Structure):
    _fields_ = [
        ("ReadOperationCount", ctypes.c_ulonglong),
        ("WriteOperationCount", ctypes.c_ulonglong),
        ("OtherOperationCount", ctypes.c_ulonglong),
        ("ReadTransferCount", ctypes.c_ulonglong),
        ("WriteTransferCount", ctypes.c_ulonglong),
        ("OtherTransferCount", ctypes.c_ulonglong),
    ]


class JOBOBJECT_BASIC_LIMIT_INFORMATION(ctypes.Structure):
    _fields_ = [
        ("PerProcessUserTimeLimit", ctypes.c_longlong),
        ("PerJobUserTimeLimit", ctypes.c_longlong),
        ("LimitFlags", wintypes.DWORD),
        ("MinimumWorkingSetSize", ctypes.c_size_t),
        ("MaximumWorkingSetSize", ctypes.c_size_t),
        ("ActiveProcessLimit", wintypes.DWORD),
        ("Affinity", ctypes.c_size_t),
        ("PriorityClass", wintypes.DWORD),
        ("SchedulingClass", wintypes.DWORD),
    ]


class JOBOBJECT_EXTENDED_LIMIT_INFORMATION(ctypes.Structure):
    _fields_ = [
        ("BasicLimitInformation", JOBOBJECT_BASIC_LIMIT_INFORMATION),
        ("IoInfo", IO_COUNTERS),
        ("ProcessMemoryLimit", ctypes.c_size_t),
        ("JobMemoryLimit", ctypes.c_size_t),
        ("PeakProcessMemoryUsed", ctypes.c_size_t),
        ("PeakJobMemoryUsed", ctypes.c_size_t),
    ]


_kernel32.CreateJobObjectW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR]
_kernel32.CreateJobObjectW.restype = wintypes.HANDLE

_kernel32.SetInformationJobObject.argtypes = [
    wintypes.HANDLE,
    wintypes.INT,
    ctypes.c_void_p,
    wintypes.DWORD,
]
_kernel32.SetInformationJobObject.restype = wintypes.BOOL

_kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
_kernel32.OpenProcess.restype = wintypes.HANDLE

_kernel32.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
_kernel32.AssignProcessToJobObject.restype = wintypes.BOOL

_kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
_kernel32.CloseHandle.restype = wintypes.BOOL


class WindowsJobObject:
    """Owns a Windows Job Object handle and can assign a process to it."""

    def __init__(self, handle: wintypes.HANDLE):
        self._handle = handle

    @classmethod
    def create_and_assign(cls, pid: int) -> "WindowsJobObject":
        if pid <= 0:
            raise OSError(f"failed to assign Windows Job Object: invalid pid {pid}")

        job = _kernel32.CreateJobObjectW(None, None)
        if not job:
            raise OSError(f"failed to create Windows Job Object: {ctypes.WinError(ctypes.get_last_error())}")

        try:
            info = JOBOBJECT_EXTENDED_LIMIT_INFORMATION()
            info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE

            ok = _kernel32.SetInformationJobObject(
                job,
                JOB_OBJECT_EXTENDED_LIMIT_INFORMATION,
                ctypes.byref(info),
                ctypes.sizeof(info),
            )
            if not ok:
                raise OSError(
                    f"failed to configure Windows Job Object: {ctypes.WinError(ctypes.get_last_error())}"
                )

            proc = _kernel32.OpenProcess(PROCESS_TERMINATE | PROCESS_SET_QUOTA, False, int(pid))
            if not proc:
                raise OSError(
                    f"failed to open process for Windows Job assignment, pid={pid}: "
                    f"{ctypes.WinError(ctypes.get_last_error())}"
                )

            try:
                ok = _kernel32.AssignProcessToJobObject(job, proc)
                if not ok:
                    raise OSError(
                        f"failed to assign process to Windows Job Object, pid={pid}: "
                        f"{ctypes.WinError(ctypes.get_last_error())}"
                    )
            finally:
                _kernel32.CloseHandle(proc)

            return cls(job)
        except Exception:
            _kernel32.CloseHandle(job)
            raise

    def close(self) -> None:
        if self._handle:
            _kernel32.CloseHandle(self._handle)
            self._handle = None
