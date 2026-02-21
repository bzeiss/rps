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
- **Allowed Dependencies**:
  - The C++ Standard Template Library (STL).
  - **Boost** (specifically `Boost.Process`, `Boost.Interprocess`, `Boost.JSON`, `Boost.Program_options`, `Boost.Filesystem`).
  - **SQLite3** (for the final output database).
  - Plugin SDK Headers (VST3, CLAP, LV2, AU, AAX).
- No other third-party frameworks (e.g., JUCE, gRPC, Protobuf, nlohmann/json) are permitted.

---

## 3. Current Implementation Status (Phase 1)

We have successfully completed **Phase 1: Project Skeleton & IPC Foundation**. The fundamental bidirectional communication and crash-recovery logic is in place and tested.

### 3.1 Project Structure
- `apps/rps-pluginscanorchestrator/`: The main orchestrator application.
- `apps/rps-pluginscanner/`: The worker application.
- `libs/rps-ipc/`: A static library containing the shared IPC transport layer and JSON serialization logic.
- `libs/rps-core/`: A header-only library for shared utilities (currently empty, reserved for future use).

### 3.2 The IPC Protocol Schema
Located in `libs/rps-ipc/include/rps/ipc/Messages.hpp`.

Instead of binary serialization like Protobuf, we chose to serialize our IPC messages as **JSON strings** using `Boost.JSON`. This provides clean, extensible, and easily debuggable messages. If a scanner crashes, we can easily inspect the last JSON payload sent over the pipe.

The protocol consists of a wrapper `Message` struct containing a variant payload:
- `ScanRequest`: Sent by Orchestrator -> Scanner. Contains the path to the `.dll`/`.so`.
- `ProgressEvent`: Sent by Scanner -> Orchestrator. Contains percentage and status strings (e.g., "Instantiating Plugin"). Crucial for resetting the Orchestrator's watchdog timer.
- `ScanResult`: Sent by Scanner -> Orchestrator on success. Contains plugin metadata.
- `ErrorMessage`: Sent by Scanner -> Orchestrator on caught, non-fatal errors.

### 3.3 The IPC Transport Layer
Located in `libs/rps-ipc/include/rps/ipc/Connection.hpp`.

We use **Boost.Interprocess** `message_queue` for fast, local, cross-platform communication. 
Each worker is assigned a unique UUID by the orchestrator at startup. The orchestrator spawns two queues per worker (one for TX, one for RX) named using that UUID. The scanner receives this UUID via command-line arguments and connects to the existing queues.

### 3.4 Crash Isolation and Watchdog Logic
The core feature of RPS is already implemented in `apps/rps-pluginscanorchestrator/src/main.cpp`:

1. The orchestrator uses `boost::process` to spawn the scanner and passes the IPC UUID.
2. It enters a polling loop using `receiveMessage(100ms)`.
3. Every time it receives a `ProgressEvent`, it updates a `lastResponseTime` timestamp.
4. If it receives nothing, it checks if `scannerProc.running()` is still true. If the process died (e.g., a **Segmentation Fault**), the orchestrator catches it gracefully and exits the loop.
5. If the process is still running but the time since `lastResponseTime` exceeds a defined `--timeout` (e.g., 10 seconds), the orchestrator assumes the plugin is **hung/deadlocked**, calls `scannerProc.terminate()`, and gracefully moves on.

*You can test this yourself by running:*
```bash
./rps-pluginscanorchestrator.exe --scan "CRASH_ME"
./rps-pluginscanorchestrator.exe --scan "HANG_ME" --timeout 2000
```

---

## 4. Next Steps (Phase 2 & 3)

The immediate next goals are:
1. **Phase 2**: Implement the **Process Pool Manager** in the orchestrator to scan multiple files in parallel, and add recursive filesystem discovery to find the actual `.dll`/`.so` files.
2. **Phase 3**: Define the `IPluginFormatScanner` interface inside the worker and write the actual code to load and parse **CLAP** and **VST3** binaries natively without JUCE.
