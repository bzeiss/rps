# RPS Developer Guide

Welcome to the **Reliable Plugin Scanner (RPS)** project! This guide explains the architecture, design choices, and implementation details of what we have built so far, serving as an onboarding manual for new developers.

---

## 1. Core Architecture

The audio plugin ecosystem is heavily reliant on dynamically loaded libraries (`.dll`, `.so`, `.dylib`). These libraries execute arbitrary, third-party C/C++ code inside the host's memory space. If a plugin has a bug, deadlocks during an iLok license check, or simply crashes, it will take the host application down with it.

RPS solves this problem by using a strict **multi-process architecture**:

1. **`rps-pluginscanorchestrator`** (The Coordinator)
   - Discovers plugin files on the filesystem.
   - Manages a pool of child worker processes.
   - Implements a watchdog timer. If a worker stops responding or crashes, the orchestrator detects the failure, logs it, and continues scanning the rest of the plugins.
   - Aggregates the final scan results into a central SQLite database.

2. **`rps-pluginscanner`** (The Worker)
   - A disposable, isolated process. 
   - It connects to the orchestrator via IPC (Inter-Process Communication), receives a `ScanRequest`, and loads a single target plugin into its own memory space.
   - It extracts the necessary metadata (name, parameters, inputs/outputs) and reports it back to the orchestrator via `ProgressEvent` and `ScanResult` messages.

---

## 2. Technology Stack & Constraints

We adhere to a set of strict constraints to ensure maximum compatibility, adoption, and reliability in the audio industry:

- **Language**: Strictly **Modern C++23**. (C++ is the undisputed standard in audio programming).
- **Build System**: **CMake** (v3.25+).
- **Compilers**: The project must build cleanly with **Clang**, **GCC**, and **MSVC**.
- **Allowed Dependencies**:
  - The C++ Standard Template Library (STL).
  - **Boost 1.90** (specifically `Boost.Process`, `Boost.Interprocess`, `Boost.JSON`, `Boost.Program_options`, `Boost.Filesystem`).
  - **SQLite3** (for the final output database).
  - Plugin SDK Headers (VST3, CLAP, LV2, AU, AAX, and optionally VST2.4).
- No other third-party frameworks (e.g., JUCE, gRPC, Protobuf, nlohmann/json) are permitted.

### 2.1 Dependency Management

To avoid platform-specific packaging issues (missing static libs on Linux, DLL mismatches on Windows, version conflicts), **Boost is built from source** as part of the CMake build. The developer must provide a Boost 1.90 source tree (cloned separately) and point CMake at it via `BOOST_SOURCE_DIR`.

The CMake variable `BOOST_SOURCE_DIR` controls where Boost source is located:
- **Default fallback**: `${CMAKE_SOURCE_DIR}/third_party/boost`
- **Override via CMake flag**: `cmake -DBOOST_SOURCE_DIR=/path/to/boost -B build`
- **Override via environment variable**: `export BOOST_SOURCE_DIR=/path/to/boost && cmake -B build`

The `-D` flag takes priority over the environment variable. If neither is set, CMake falls back to `third_party/boost`.

Only the required Boost libraries are compiled (via `BOOST_INCLUDE_LIBRARIES`), keeping build times reasonable.

On **Windows** (non-MSVC), the C/C++ runtimes are statically linked (`-static -static-libgcc -static-libstdc++`) to produce standalone executables with no DLL dependencies beyond the Windows system libraries.

### 2.2 Optional: VST2.4 SDK

VST2 scanning is gated behind a **compile-time CMake option** (`RPS_ENABLE_VST2`, default OFF) because the VST2.4 SDK is proprietary and cannot be redistributed.

When enabled, the build expects the SDK at `libs/rps-core/include/rps/core/vstsdk2.4/` (or a custom path via `-DRPS_VST2_SDK_PATH`). The key header is `pluginterfaces/vst2.x/aeffect.h`.

```bash
# Enable VST2 with default SDK location:
cmake -G Ninja -DRPS_ENABLE_VST2=ON -B build

# Enable VST2 with custom SDK path:
cmake -G Ninja -DRPS_ENABLE_VST2=ON -DRPS_VST2_SDK_PATH=/path/to/vstsdk2.4 -B build
```

When OFF, no VST2 code is compiled or linked. The compile definition `RPS_VST2_ENABLED` controls `#ifdef` guards in `Vst2Scanner.hpp`, `Vst2Scanner.cpp`, and `main.cpp`.

At runtime, VST2 is **excluded from `--formats all`** via the `isExplicitOnly()` trait. Users must explicitly pass `--formats vst2` to scan VST2 plugins. This prevents accidentally scanning thousands of unrelated `.dll` files in legacy VST2 directories.

---

## 3. Current Implementation Status (Phases 1-4)

We have successfully completed **Phase 1 (IPC Foundation)**, **Phase 2 (Process Pool & Orchestration)**, **Phase 3 (Format Scanners)**, and **Phase 4 (Incremental Scanning & Database Optimization)**. The fundamental bidirectional communication, crash-recovery logic, targeted format scanning, and incremental caching are in place and tested against thousands of real-world plugins.

### 3.1 Project Structure
- `apps/rps-pluginscanorchestrator/`: The main orchestrator application. Manages parallel worker pools, applies format/name filtering, and writes to SQLite.
- `apps/rps-pluginscanner/`: The worker application. Loads the plugin DLL natively.
- `libs/rps-ipc/`: A static library containing the shared IPC transport layer and JSON serialization logic.
- `libs/rps-core/`: Format traits, registry, and filesystem discovery logic.

### 3.2 The IPC Protocol Schema
Located in `libs/rps-ipc/include/rps/ipc/Messages.hpp`.

Instead of binary serialization like Protobuf, we chose to serialize our IPC messages as **JSON strings** using `Boost.JSON`. This provides clean, extensible, and easily debuggable messages. If a scanner crashes, we can easily inspect the last JSON payload sent over the pipe.

The protocol consists of a wrapper `Message` struct containing a variant payload:
- `ScanRequest`: Sent by Orchestrator -> Scanner. Contains the path to the `.dll`/`.so`.
- `ProgressEvent`: Sent by Scanner -> Orchestrator. Contains percentage and status strings (e.g., "Instantiating Plugin"). Crucial for resetting the Orchestrator's watchdog timer.
- `ScanResult`: Sent by Scanner -> Orchestrator on success. Contains rich DAW-ready metadata: Name, Vendor, Version, UID, Description, URL, Categories, and Parameter lists.
- `ErrorMessage`: Sent by Scanner -> Orchestrator on caught, non-fatal errors.

### 3.3 The IPC Transport Layer
Located in `libs/rps-ipc/include/rps/ipc/Connection.hpp`.

We use **Boost.Interprocess** `message_queue` for fast, local, cross-platform communication. 
Each worker is assigned a unique UUID by the orchestrator at startup. The orchestrator spawns two queues per worker (one for TX, one for RX) named using that UUID. The scanner receives this UUID via command-line arguments and connects to the existing queues.

### 3.4 Crash Isolation, Watchdog Logic, and UI Suppression
The core feature of RPS is fully implemented in `ProcessPool.cpp` and `scanner/main.cpp`:

1. The orchestrator uses `boost::process` to spawn the scanner and passes the IPC UUID.
2. **UI Suppression (Windows)**: The orchestrator assigns the scanner child process to a Windows Job Object with `JOB_OBJECT_UILIMIT_HANDLES`. This silently blocks the plugin from creating Win32 `MessageBox` dialogs (which commonly happen during license/mutex failures) that would otherwise block the scanner indefinitely. The scanner itself also calls `SetErrorMode` to suppress OS-level crash dialogs.
3. The orchestrator enters a polling loop. Every time it receives a `ProgressEvent`, it updates a `lastResponseTime` timestamp.
4. If the process dies (e.g., a **Segmentation Fault** / `0xC0000005`), the orchestrator catches it gracefully, logs the exit code, and moves on.
5. If the process is still running but the time since `lastResponseTime` exceeds a defined `--timeout`, the orchestrator assumes the plugin is **hung/deadlocked**, calls `scannerProc.terminate()`, and gracefully moves on.

### 3.5 Scanner Process Lifecycle
After extracting metadata, the scanner process must exit as fast as possible. Plugin DLLs frequently hang during cleanup (`DLL_PROCESS_DETACH` on Windows, `dlclose` callbacks on Linux/macOS). RPS handles this with a two-sided approach:

- **Scanner side**: On Windows, the scanner calls `TerminateProcess(GetCurrentProcess(), 0)` instead of `_exit(0)` to skip `DLL_PROCESS_DETACH` handlers entirely. On other platforms, `_exit(0)` suffices.
- **Orchestrator side**: After receiving the scan result via IPC, the orchestrator immediately calls `terminate()` on the child process (no grace period) and explicitly closes the stderr pipe to unblock the drainer thread. This eliminates any post-scan delay.

### 3.6 Database Performance
SQLite writes are wrapped in explicit `BEGIN TRANSACTION / COMMIT` blocks. Without this, each parameter INSERT is an implicit transaction with its own `fsync` -- a plugin with 2000+ parameters would take 30-50 seconds to persist. With explicit transactions, the same operation completes in under 50ms.

The database uses **WAL (Write-Ahead Logging)** journal mode, enabled at schema initialization. WAL allows concurrent reads while writes are in progress.

### 3.7 Incremental Scanning
RPS supports two scan modes via the `--mode` CLI flag:

- **`incremental`** (default): Loads a cache of previously scanned plugins from the database. For each discovered plugin file, it compares the file's modification time (`last_write_time`) against the cached value. If the mtime matches, it further compares a CRC32 hash (via `boost/crc.hpp`). Only plugins that are new or changed are dispatched for scanning. Plugins that no longer exist on disk are automatically pruned from the database.

- **`full`**: Clears database entries for the requested format(s) only, then rescans everything. A `--mode full --formats vst2` will clear only VST2 entries, leaving CLAP/VST3 data intact.

The `format` column in the `plugins` table (set to `"vst2"`, `"vst3"`, or `"clap"` by each scanner) enables this per-format isolation. File metadata (`file_mtime`, `file_hash`) is computed by the orchestrator before dispatching each scan job and stored alongside the scan results.

### 3.8 Build Robustness & Static Linking
To ensure the orchestrator and scanner binaries are portable and do not fail with "missing DLL" errors (exit code 127 on Windows), we strictly **statically link** the C/C++ runtimes (`-static-libgcc -static-libstdc++`), Boost, and SQLite. On Windows, we use the MSYS2 Clang64 environment for the most compliant C++23 build.

---

## 4. Next Steps (Phase 5+)

The immediate next goals are:
1. **OS-Specific Formats**: Implement scanners for **AU** (macOS via CoreAudio) and **LV2** (Linux/Windows).
2. **AAX**: Requires the PACE/Avid SDK for proper scanning.
3. **Extended Metadata**: Store additional plugin details (e.g., bus arrangements, preset lists) if exposed by the SDKs.
4. **Vendor SQLite**: Bundle the SQLite amalgamation to remove the system dependency.
