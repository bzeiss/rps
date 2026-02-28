package rps.client;

import java.io.IOException;
import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.SymbolLookup;
import java.lang.invoke.MethodHandle;

final class WindowsJobObject implements AutoCloseable {
    private static final int JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x00002000;
    private static final int JOB_OBJECT_EXTENDED_LIMIT_INFORMATION = 9;
    private static final int LIMIT_FLAGS_OFFSET = 16; // DWORD LimitFlags
    private static final int PROCESS_TERMINATE = 0x0001;
    private static final int PROCESS_SET_QUOTA = 0x0100;
    private static final int POINTER_SIZE = (int) java.lang.foreign.ValueLayout.ADDRESS.byteSize();
    private static final int JOB_OBJECT_EXTENDED_LIMIT_INFORMATION_SIZE;

    private static final MethodHandle CREATE_JOB_OBJECT_A;
    private static final MethodHandle SET_INFORMATION_JOB_OBJECT;
    private static final MethodHandle OPEN_PROCESS;
    private static final MethodHandle ASSIGN_PROCESS_TO_JOB_OBJECT;
    private static final MethodHandle CLOSE_HANDLE;
    private static final MethodHandle GET_LAST_ERROR;
    private static final boolean PROCESS_DEBUG =
            "1".equals(System.getenv("RPS_DEBUG_PROCESS_LIFECYCLE"));

    static {
        int minimumWorkingSetOffset = alignUp(20, POINTER_SIZE);
        int maximumWorkingSetOffset = minimumWorkingSetOffset + POINTER_SIZE;
        int activeProcessLimitOffset = maximumWorkingSetOffset + POINTER_SIZE;
        int affinityOffset = alignUp(activeProcessLimitOffset + 4, POINTER_SIZE);
        int priorityClassOffset = affinityOffset + POINTER_SIZE;
        int schedulingClassOffset = priorityClassOffset + 4;
        int basicLimitSize = schedulingClassOffset + 4;
        int ioCountersSize = 48; // 6x ULONGLONG
        int processMemoryLimitSize = POINTER_SIZE;
        int jobMemoryLimitSize = POINTER_SIZE;
        int peakProcessMemoryUsedSize = POINTER_SIZE;
        int peakJobMemoryUsedSize = POINTER_SIZE;
        JOB_OBJECT_EXTENDED_LIMIT_INFORMATION_SIZE =
                basicLimitSize + ioCountersSize + processMemoryLimitSize + jobMemoryLimitSize
                        + peakProcessMemoryUsedSize + peakJobMemoryUsedSize;
    }

    static {
        try {
            Linker linker = Linker.nativeLinker();
            SymbolLookup kernel32 = SymbolLookup.libraryLookup("kernel32", Arena.global());

            CREATE_JOB_OBJECT_A = linker.downcallHandle(
                    kernel32.find("CreateJobObjectA").orElseThrow(),
                    FunctionDescriptor.of(
                            java.lang.foreign.ValueLayout.ADDRESS,
                            java.lang.foreign.ValueLayout.ADDRESS,
                            java.lang.foreign.ValueLayout.ADDRESS
                    )
            );
            SET_INFORMATION_JOB_OBJECT = linker.downcallHandle(
                    kernel32.find("SetInformationJobObject").orElseThrow(),
                    FunctionDescriptor.of(
                            java.lang.foreign.ValueLayout.JAVA_INT,
                            java.lang.foreign.ValueLayout.ADDRESS,
                            java.lang.foreign.ValueLayout.JAVA_INT,
                            java.lang.foreign.ValueLayout.ADDRESS,
                            java.lang.foreign.ValueLayout.JAVA_INT
                    )
            );
            OPEN_PROCESS = linker.downcallHandle(
                    kernel32.find("OpenProcess").orElseThrow(),
                    FunctionDescriptor.of(
                            java.lang.foreign.ValueLayout.ADDRESS,
                            java.lang.foreign.ValueLayout.JAVA_INT,
                            java.lang.foreign.ValueLayout.JAVA_INT,
                            java.lang.foreign.ValueLayout.JAVA_INT
                    )
            );
            ASSIGN_PROCESS_TO_JOB_OBJECT = linker.downcallHandle(
                    kernel32.find("AssignProcessToJobObject").orElseThrow(),
                    FunctionDescriptor.of(
                            java.lang.foreign.ValueLayout.JAVA_INT,
                            java.lang.foreign.ValueLayout.ADDRESS,
                            java.lang.foreign.ValueLayout.ADDRESS
                    )
            );
            CLOSE_HANDLE = linker.downcallHandle(
                    kernel32.find("CloseHandle").orElseThrow(),
                    FunctionDescriptor.of(
                            java.lang.foreign.ValueLayout.JAVA_INT,
                            java.lang.foreign.ValueLayout.ADDRESS
                    )
            );
            GET_LAST_ERROR = linker.downcallHandle(
                    kernel32.find("GetLastError").orElseThrow(),
                    FunctionDescriptor.of(java.lang.foreign.ValueLayout.JAVA_INT)
            );
        } catch (Throwable t) {
            throw new ExceptionInInitializerError(t);
        }
    }

    private final MemorySegment jobHandle;

    private WindowsJobObject(MemorySegment jobHandle) {
        this.jobHandle = jobHandle;
    }

    static WindowsJobObject createAndAssign(long pid) throws IOException {
        if (pid <= 0 || pid > Integer.MAX_VALUE) {
            throw new IOException("Unsupported process id: " + pid);
        }

        try {
            MemorySegment jobHandle = (MemorySegment) CREATE_JOB_OBJECT_A.invokeExact(
                    MemorySegment.NULL,
                    MemorySegment.NULL
            );
            if (MemorySegment.NULL.equals(jobHandle)) {
                throw new IOException("failed to create Windows Job Object (GetLastError=" + getLastError() + ")");
            }

            try (Arena arena = Arena.ofConfined()) {
                MemorySegment info = arena.allocate(JOB_OBJECT_EXTENDED_LIMIT_INFORMATION_SIZE);
                info.set(java.lang.foreign.ValueLayout.JAVA_INT, LIMIT_FLAGS_OFFSET, JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE);

                int setRes = (int) SET_INFORMATION_JOB_OBJECT.invokeExact(
                        jobHandle,
                        JOB_OBJECT_EXTENDED_LIMIT_INFORMATION,
                        info,
                        JOB_OBJECT_EXTENDED_LIMIT_INFORMATION_SIZE
                );
                if (setRes == 0) {
                    throw new IOException(
                            "failed to configure Windows Job Object (GetLastError=" + getLastError()
                                    + ", infoSize=" + JOB_OBJECT_EXTENDED_LIMIT_INFORMATION_SIZE
                                    + ", ptrSize=" + POINTER_SIZE + ")"
                    );
                }

                MemorySegment processHandle = (MemorySegment) OPEN_PROCESS.invokeExact(
                        PROCESS_TERMINATE | PROCESS_SET_QUOTA,
                        0,
                        (int) pid
                );
                if (MemorySegment.NULL.equals(processHandle)) {
                    throw new IOException("failed to open process for Windows Job assignment, pid=" + pid
                            + " (GetLastError=" + getLastError() + ")");
                }

                try {
                    int assignRes = (int) ASSIGN_PROCESS_TO_JOB_OBJECT.invokeExact(jobHandle, processHandle);
                    if (assignRes == 0) {
                        throw new IOException("failed to assign process to Windows Job Object, pid=" + pid
                                + " (GetLastError=" + getLastError() + ")");
                    }
                } finally {
                    closeHandleQuiet(processHandle);
                }
            } catch (Throwable t) {
                closeHandleQuiet(jobHandle);
                if (t instanceof IOException ioe) {
                    throw ioe;
                }
                throw new IOException("failed to configure Windows Job Object", t);
            }

            if (PROCESS_DEBUG) {
                System.out.println("[rps] created Windows Job Object for pid " + pid);
            }
            return new WindowsJobObject(jobHandle);
        } catch (IOException e) {
            throw e;
        } catch (Throwable t) {
            throw new IOException("failed to initialize Windows Job Object", t);
        }
    }

    @Override
    public void close() {
        closeHandleQuiet(jobHandle);
    }

    private static void closeHandleQuiet(MemorySegment handle) {
        if (handle == null || MemorySegment.NULL.equals(handle)) {
            return;
        }
        try {
            CLOSE_HANDLE.invoke(handle);
        } catch (Throwable ignored) {
        }
    }

    private static int getLastError() {
        try {
            return (int) GET_LAST_ERROR.invokeExact();
        } catch (Throwable ignored) {
            return -1;
        }
    }

    private static int alignUp(int value, int alignment) {
        int mask = alignment - 1;
        return (value + mask) & ~mask;
    }
}
