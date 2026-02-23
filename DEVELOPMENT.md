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
  - Plugin SDK Headers (VST3, CLAP, LV2, AU, AAX).
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

---

## 3. Current Implementation Status (Phases 1-3)

We have successfully completed **Phase 1 (IPC Foundation)**, **Phase 2 (Process Pool & Orchestration)**, and are deep into **Phase 3 (Format Scanners)**. The fundamental bidirectional communication, crash-recovery logic, and targeted format scanning are in place and tested against thousands of real-world plugins.

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

### 3.5 Build Robustness & Static Linking
To ensure the orchestrator and scanner binaries are portable and do not fail with "missing DLL" errors (exit code 127 on Windows), we strictly **statically link** the C/C++ runtimes (`-static-libgcc -static-libstdc++`), Boost, and SQLite. On Windows, we use the MSYS2 Clang64 environment for the most compliant C++23 build.

---

## 4. Next Steps (Phase 3 & 4)

The immediate next goals are:
1. **Phase 3 (OS Formats)**: Implement scanners for OS-specific formats (**AU** on macOS via CoreAudio, **LV2** on Linux/Windows).
2. **Phase 3 (Legacy Formats)**: Implement legacy formats (**VST2**, **AAX**). Note that VST2 requires deprecated Steinberg SDK headers, and AAX requires the PACE/Avid SDK.
3. **Phase 4**: Expand the SQLite database to store even more plugin details (e.g., bus arrangements, preset lists) if exposed by the SDKs.
