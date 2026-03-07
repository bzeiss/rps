# RPS Developer Guide

Welcome to the **RPS** project! This guide explains the architecture, design choices, and implementation details of what we have built so far, serving as an onboarding manual for new developers.

---

## 1. Core Architecture

The audio plugin ecosystem is heavily reliant on dynamically loaded libraries (`.dll`, `.so`, `.dylib`). These libraries execute arbitrary, third-party C/C++ code inside the host's memory space. If a plugin has a bug, deadlocks during an iLok license check, or simply crashes, it will take the host application down with it.

RPS solves this problem by using a strict **multi-process architecture**:

1. **`rps-engine`** (The Core Library)
   - A reusable static library (`libs/rps-engine/`) containing all scan orchestration logic: plugin discovery, process pool management, watchdog timers, retry logic, incremental caching, and SQLite persistence.
   - Exposes `ScanEngine::runScan(ScanConfig, ScanObserver*)` — a single entry point for any consumer.

2. **`rps-server`** (The gRPC Server)
   - A long-lived daemon that exposes `rps-engine` via a gRPC streaming API.
   - Implements `GrpcScanObserver` to serialize scan events into protobuf messages streamed to clients.
   - Logs to console and file via `spdlog`.
   - Can be driven from any language (Python, C++, etc.).

3. **`rps-standalone`** (Standalone CLI)
   - A thin CLI wrapper (~100 lines) around `rps-engine`. No server needed.

4. **`vstscannermaster`** (VST3 Scanner Master)
   - Drop-in replacement for Steinberg's `vstscannermaster.exe`.
   - Accepts the same CLI arguments (`-prefPath`, `-licenceLevel`, `-hostName`, `-progress`, `-rescan`, `-timeout`).
   - Uses `rps-engine` for crash-isolated VST3 scanning, then reads the SQLite DB and writes Steinberg-compatible XML cache files (`vst3plugins.xml`, `vst3blocklist.xml`, `vst3allowlist.xml`).

5. **`rps-pluginscanner`** (The Worker)
   - A disposable, isolated process.
   - It connects to the engine via IPC (Inter-Process Communication), receives a `ScanRequest`, and loads a single target plugin into its own memory space.
   - It extracts the necessary metadata (name, parameters, inputs/outputs) and reports it back via `ProgressEvent` and `ScanResult` messages.

6. **`rps-pluginhost-vst3`** (VST3 GUI Host Worker)
   - An isolated process that loads a VST3 plugin, creates an SDL3 window, and embeds the plugin's native editor view.
   - Features an ImGui preset browser sidebar (collapsible, searchable, multi-column table).
   - Discovers presets from `IUnitInfo` (`IProgramListData`) and `.vstpreset` file scanning across standard directories (`%APPDATA%/VST3 Presets/`, `Documents/VST3 Presets/`, `%COMMONPROGRAMFILES%/VST3 Presets/`) with vendor-agnostic matching.
   - Loads presets via `IUnitInfo` + minimal audio process pass, or `.vstpreset` file parsing (inline binary parser for component/controller state chunks).
   - Communicates with the server via the same IPC message queues.

7. **`rps-pluginhost-clap`** (CLAP GUI Host Worker)
   - An isolated process that loads a CLAP plugin, creates an SDL3 window, and embeds the plugin's native editor view.
   - Same ImGui preset sidebar architecture as the VST3 host.
   - Discovers and loads presets via the CLAP preset-load extension.
   - Communicates with the server via IPC message queues.

---

## 2. Technology Stack & Constraints

We adhere to a set of strict constraints to ensure maximum compatibility, adoption, and reliability in the audio industry:

- **Language**: Strictly **Modern C++23**. (C++ is the undisputed standard in audio programming).
- **Build System**: **CMake** (v3.25+).
- **Compilers**: The project must build cleanly with **Clang**, **GCC**, and **MSVC**.
- **Allowed Dependencies**:
  - The C++ Standard Template Library (STL).
  - **Boost 1.90** (specifically `Boost.Process`, `Boost.Interprocess`, `Boost.JSON`, `Boost.Program_options`, `Boost.Filesystem`, `Boost.UUID`).
  - **SQLite3** (for the final output database).
  - **gRPC / Protobuf** (for the `rps-server` API layer).
  - **spdlog** (structured logging for `rps-server`).
  - Plugin SDK Headers (VST3, CLAP, LV2, AU, AAX, and optionally VST2.4).
  - **SDL3** (for plugin GUI hosting windows).
  - **Dear ImGui** (for the preset browser sidebar UI).
  - No other third-party frameworks (e.g., JUCE, nlohmann/json) are permitted in the core engine.

### 2.1 Dependency Management

To guarantee binary portability (creating self-contained executables without DLL/shared object dependencies), RPS relies on **`vcpkg`** across all platforms for resolving its dependencies (`boost`, `grpc`, `protobuf`, `spdlog`, and `sqlite3`) as **static libraries**.

- **Windows**: Built with MSVC (`cl.exe`) or Clang via the Ninja generator, using the `x64-windows-static` vcpkg triplet and `-DRPS_MSVC_STATIC_RUNTIME=ON`.
- **macOS**: Built with Apple Clang using the `arm64-osx-static` vcpkg triplet.
- **Linux**: Built with GCC using the `x64-linux` vcpkg triplet (which defaults to static linking).

Because all dependencies are managed via `vcpkg.json` (Manifest Mode), the CMake configure step will automatically download and build the exact requested versions of Boost (1.90.0) and other libraries before compiling RPS.

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

## 3. Current Implementation Status (Phases 1-6)

We have successfully completed **Phase 1 (IPC Foundation)**, **Phase 2 (Process Pool & Orchestration)**, **Phase 3 (Format Scanners)**, **Phase 4 (Incremental Scanning & Database Optimization)**, **Phase 5 (AAX Scanner & Robustness)**, and **Phase 6 (gRPC Server & Python TUI Client)**. The full stack — from IPC to gRPC streaming to a rich Python TUI — is working end-to-end and tested against thousands of real-world plugins.

### 3.1 Project Structure

#### Libraries
- `libs/rps-core/`: Format traits, registry, and filesystem discovery logic.
- `libs/rps-ipc/`: Shared IPC transport layer and JSON serialization logic (engine ↔ scanner/host).
- `libs/rps-engine/`: **The reusable scan engine library** (namespace `rps::engine`). Contains all orchestration logic extracted from the original CLI app:
  - `ScanEngine.hpp/.cpp`: Top-level `ScanEngine` class with `runScan(ScanConfig, ScanObserver*)`. Thread-safe (one scan at a time).
  - `ScanConfig`: Struct encapsulating all scan parameters (formats, mode, jobs, timeout, etc.).
  - `ProcessPool.hpp/.cpp`: Worker process lifecycle, IPC message loop, retry logic, watchdog.
  - `ScanObserver.hpp`: Abstract observer interface for scan progress events.
  - `ConsoleScanObserver.hpp/.cpp`: Console-based observer for CLI use.
  - `db/DatabaseManager.hpp/.cpp`: SQLite database layer — schema, upserts, incremental cache, skipped/blocked management.
- `libs/rps-gui/`: **Shared GUI hosting library** used by both plugin host processes:
  - `IPluginGuiHost.hpp`: Abstract interface for format-specific GUI hosts (open, event loop, params, state, presets).
  - `SdlWindow.hpp/.cpp`: SDL3 window management with Dear ImGui integration. Implements the collapsible preset browser sidebar (search filter, multi-column sortable table), "Stationary Content" window expansion pattern, and child HWND positioning to solve the Windows Airspace Problem.
  - `GuiWorkerMain.hpp/.cpp`: Common IPC event loop for host worker processes (receives commands from the server, dispatches to `IPluginGuiHost`).

#### Applications
- `apps/rps-standalone/`: Standalone CLI wrapper. A thin `main.cpp` (~100 lines) that parses CLI args into `ScanConfig` and calls `ScanEngine::runScan()`.
- `apps/rps-pluginscanner/`: The worker application. Loads the plugin DLL natively.
  - `src/scanners/Vst3Scanner.cpp`: VST3 scanning via native COM/module loading.
  - `src/scanners/ClapScanner.cpp`: CLAP scanning via the CLAP C API.
  - `src/scanners/Vst2Scanner.cpp`: VST2 scanning (compile-time opt-in).
  - `src/scanners/AaxScanner.cpp`: AAX scanning via Pro Tools cache file parsing.
- `apps/rps-server/`: **gRPC server** exposing the scan engine and GUI hosting to any language.
  - `src/RpsServiceImpl.cpp`: Implements the `RpsService` gRPC service (StartScan, StopScan, GetStatus, Shutdown, OpenPluginGui, ClosePluginGui, ListPlugins, GetPluginState, SetPluginState, LoadPreset).
  - `src/GrpcScanObserver.cpp`: Implements `ScanObserver`, serializes events into protobuf `ScanEvent` messages and writes them to a `grpc::ServerWriter`.
  - `src/GuiSessionManager.cpp`: Manages active plugin GUI sessions. Spawns the appropriate host worker process (`rps-pluginhost-vst3` or `rps-pluginhost-clap`), establishes IPC, and relays events between the gRPC stream and the host worker.
  - Uses `spdlog` for dual-sink logging (stdout + file).

- `apps/rps-pluginhost-vst3/`: **VST3 GUI host worker** process.
  - `src/Vst3GuiHost.cpp`: Implements `IPluginGuiHost` for VST3 plugins. Handles COM module loading, `IComponent`/`IEditController` setup, `IPlugView` embedding in the SDL3 window, parameter discovery, preset discovery (via `IUnitInfo` and `.vstpreset` file scanning), and preset loading (via `IProgramListData`, audio process pass, or file-based state restore).

- `apps/rps-pluginhost-clap/`: **CLAP GUI host worker** process.
  - `src/ClapGuiHost.cpp`: Implements `IPluginGuiHost` for CLAP plugins. Handles CLAP plugin loading, GUI embedding, parameter discovery, and preset discovery/loading via the CLAP preset-load extension.

#### Proto
- `proto/rps.proto`: gRPC service and message definitions. `ScanEvent` is a `oneof` union mirroring all `ScanObserver` callbacks 1:1. Also defines `GuiEvent` messages for the GUI hosting stream (gui_opened, gui_closed, parameter_list, parameter_updates, preset_list, preset_loaded, gui_error).

#### Example Clients
- `examples/python/`: Python TUI client using `rich` for per-worker progress bars. Features an interactive `open-gui` command with an `InquirerPy` fuzzy plugin selector (arrow keys, Page-Up/Down for 20-entry jumps, cursor position memory). Spawns/kills `rps-server` as a subprocess.
- `examples/cpp/`: C++ gRPC client with ANSI terminal TUI. Same features as the Python client: auto-spawns server, per-worker progress bars, streaming results.

### 3.2 The IPC Protocol Schema
Located in `libs/rps-ipc/include/rps/ipc/Messages.hpp`.

Instead of binary serialization like Protobuf, we chose to serialize our IPC messages as **JSON strings** using `Boost.JSON`. This provides clean, extensible, and easily debuggable messages. If a scanner crashes, we can easily inspect the last JSON payload sent over the pipe.

The protocol consists of a wrapper `Message` struct containing a variant payload:
- `ScanRequest`: Sent by Engine -> Scanner. Contains the path to the `.dll`/`.so`.
- `ProgressEvent`: Sent by Scanner -> Engine. Contains percentage and status strings (e.g., "Instantiating Plugin"). Crucial for resetting the engine's watchdog timer.
- `ScanResult`: Sent by Scanner -> Engine on success. Contains rich DAW-ready metadata: Name, Vendor, Version, UID, Description, URL, Categories, Parameter lists, and an `extraData` map for format-specific metadata (e.g., AAX variant triad IDs).
- `ErrorMessage`: Sent by Scanner -> Engine on caught, non-fatal errors. Errors prefixed with `SKIP:` are treated as non-scannable plugins (stored in `plugins_skipped`).

### 3.3 The IPC Transport Layer
Located in `libs/rps-ipc/include/rps/ipc/Connection.hpp`.

We use **Boost.Interprocess** `message_queue` for fast, local, cross-platform communication. 
Each worker is assigned a unique UUID by the engine at startup. The engine spawns two queues per worker (one for TX, one for RX) named using that UUID. The scanner receives this UUID via command-line arguments and connects to the existing queues.

### 3.4 Crash Isolation, Watchdog Logic, and UI Suppression
The core feature of RPS is fully implemented in `ProcessPool.cpp` and `scanner/main.cpp`:

1. The engine uses `boost::process` to spawn the scanner and passes the IPC UUID.
2. **UI Suppression (Windows)**: The engine assigns the scanner child process to a Windows Job Object with `JOB_OBJECT_UILIMIT_HANDLES`. This silently blocks the plugin from creating Win32 `MessageBox` dialogs (which commonly happen during license/mutex failures) that would otherwise block the scanner indefinitely. The scanner itself also calls `SetErrorMode` to suppress OS-level crash dialogs.
3. The engine enters a polling loop. Every time it receives a `ProgressEvent`, it updates a `lastResponseTime` timestamp.
4. If the process dies (e.g., a **Segmentation Fault** / `0xC0000005`), the engine catches it gracefully, logs the exit code, and moves on.
5. If the process is still running but the time since `lastResponseTime` exceeds a defined `--timeout`, the engine assumes the plugin is **hung/deadlocked**, calls `scannerProc.terminate()`, and gracefully moves on.

### 3.5 Scanner Process Lifecycle
After extracting metadata, the scanner process must exit as fast as possible. Plugin DLLs frequently hang during cleanup (`DLL_PROCESS_DETACH` on Windows, `dlclose` callbacks on Linux/macOS). RPS handles this with a two-sided approach:

- **Scanner side**: On Windows, the scanner calls `TerminateProcess(GetCurrentProcess(), 0)` instead of `_exit(0)` to skip `DLL_PROCESS_DETACH` handlers entirely. On other platforms, `_exit(0)` suffices.
- **Engine side**: After receiving the scan result via IPC, the engine immediately calls `terminate()` on the child process (no grace period) and explicitly closes the stderr pipe to unblock the drainer thread. This eliminates any post-scan delay.

### 3.6 Database Performance
SQLite writes are wrapped in explicit `BEGIN TRANSACTION / COMMIT` blocks. Without this, each parameter INSERT is an implicit transaction with its own `fsync` -- a plugin with 2000+ parameters would take 30-50 seconds to persist. With explicit transactions, the same operation completes in under 50ms.

The database uses **WAL (Write-Ahead Logging)** journal mode, enabled at schema initialization. WAL allows concurrent reads while writes are in progress.

### 3.7 Incremental Scanning
RPS supports two scan modes via the `--mode` CLI flag:

- **`incremental`** (default): Loads caches from three tables (`plugins`, `plugins_skipped`, `plugins_blocked`) filtered by the requested formats. For each discovered plugin file, it compares the file's modification time (`last_write_time`) against the cached value. Only plugins that are new or changed are dispatched for scanning. Plugins that no longer exist on disk are automatically pruned from all tables.

- **`full`**: Clears database entries for the requested format(s) only across all tables, then rescans everything. A `--mode full --formats vst2` will clear only VST2 entries, leaving CLAP/VST3 data intact.

All cache loading and stale-entry removal is **format-aware**: scanning `--formats vst3` will never touch AAX or CLAP data in the database. The `format` column in each table enables this isolation.

### 3.8 Skipped & Blocked Plugin Persistence

Plugins that cannot be scanned (e.g., empty VST3 bundles, no loadable binary for the current platform) return a `SKIP:` prefixed error from the scanner. These are stored in the `plugins_skipped` table with their `file_mtime`. On subsequent incremental scans, they are recognized and not dispatched to a worker process — avoiding unnecessary process spawns.

Plugins that exhaust all retry attempts (default: 3) or time out are stored in the `plugins_blocked` table. These are also skipped on subsequent incremental scans. If the plugin file is updated (mtime changes), it is automatically unblocked and re-scanned.

Both tables are cleared when running `--mode full` for the relevant formats.

### 3.9 AAX Scanner

AAX plugins are iLok-protected and cannot be loaded directly. Instead, `AaxScanner` parses Pro Tools' plugin cache files (`*.plugincache.txt`), which are generated by Pro Tools itself during its own plugin scan.

Cache file search paths:
- **Windows**: `C:\Users\Public\Pro Tools\AAXPlugInCache`, `%APPDATA%\Avid\Pro Tools`
- **macOS**: `/Users/Shared/Pro Tools/AAXPluginCache`, `~/Library/Preferences/Avid/Pro Tools`

The cache files use a tab-indented text format containing multiple plugin variants per file (different I/O configs, Native vs DSP, etc.). The scanner:
1. Parses all variants into `AaxPluginVariant` structs.
2. Picks the "best" variant for the main `ScanResult` (Native/PlugInType=3 preferred, stereo preferred).
3. Packs **all** variant data into `ScanResult.extraData` as key-value pairs (e.g., `aax_v0_manufacturer_id`, `aax_v0_product_id_num`, etc.).
4. The engine unpacks `extraData` into the `aax_plugins` table (1:N relationship with `plugins`).

FourCC codes containing non-ASCII bytes (invalid UTF-8) are sanitized to hex representation (e.g., `0xD2C0AAA5`) to keep JSON serialization safe. The numeric IDs are stored separately and are what PTSL (Pro Tools Scripting) needs.

### 3.10 Database Tables

| Table | Purpose |
|---|---|
| `plugins` | Main plugin data: one row per successfully scanned or failed plugin |
| `parameters` | Plugin parameters (1:N with `plugins`) |
| `aax_plugins` | AAX variant-specific triad IDs and stem formats (1:N with `plugins`) |
| `vst3_classes` | VST3 multi-class entries per plugin (1:N with `plugins`): `class_index`, `name`, `uid`, `category`, `vendor`, `version` |
| `plugins_skipped` | Non-scannable plugins (empty bundles, no binary) — checked during incremental scan |
| `plugins_blocked` | Plugins that exhausted retries or timed out — checked during incremental scan |

All mutating database operations are wrapped in explicit `BEGIN TRANSACTION / COMMIT` blocks for both atomicity and performance.

### 3.11 Build Robustness & Static Linking
To ensure the scanner binaries are portable and do not fail with "missing DLL" errors (exit code 127 on Windows), we strictly **statically link** the C/C++ runtimes (`-static-libgcc -static-libstdc++`), Boost, and SQLite. On Windows, we use the MSYS2 Clang64 environment for the most compliant C++23 build.

---

### 3.12 gRPC Server Architecture

The `rps-server` application exposes the `rps::engine::ScanEngine` via a gRPC service defined in `proto/rps.proto`. The key insight is that the existing `ScanObserver` abstract interface is a natural event bus — `GrpcScanObserver` implements it and serializes each callback into a protobuf `ScanEvent` message written to the gRPC response stream.

Architecture:
```
Client (Python/C++/etc.)  ──gRPC──▶  RpsServiceImpl  ──▶  ScanEngine  ──▶  ProcessPool  ──IPC──▶  rps-pluginscanner
         ◀── stream ScanEvent ──          GrpcScanObserver
```

- **One scan at a time**: `ScanEngine::m_scanning` atomic flag. Returns `ALREADY_EXISTS` gRPC status if busy.
- **Logging**: `spdlog` with two sinks (stdout + file). Current verbose `std::cerr` output → `spdlog::debug()`. Normal → `spdlog::info()`. Errors → `spdlog::error()`.
- **Shutdown**: `Shutdown` RPC triggers `grpc::Server::Shutdown()` asynchronously (after returning the response). Signal handlers (SIGINT/SIGTERM) also trigger graceful shutdown.
- **Lifecycle**: Designed to be spawned and killed by a parent application. The Python example client demonstrates this pattern.

---

### 3.13 Python TUI Client

The example Python client (`examples/python/`) demonstrates the full gRPC workflow:

1. **`ServerManager`**: Spawns `rps-server` as a subprocess, waits for it to accept connections, auto-adds MSYS2 DLL directories to subprocess PATH on Windows. Gracefully shuts down via the `Shutdown` RPC on exit.
2. **`RpsClient`**: Thin gRPC stub wrapper for `StartScan`, `StopScan`, `GetStatus`, `Shutdown`.
3. **`ScanDisplay` TUI**: Uses the `rich` library with a custom `__rich_console__` renderable to avoid flicker. Shows:
   - Overall progress bar with spinner, percentage, elapsed time, and live counters.
   - Per-worker status table with individual progress bars.
   - Scrolling log of recent scan results (success/fail/crash/timeout/skip).
4. **`open-gui` Command**: Interactive plugin GUI browser driven via gRPC:
   - Fuzzy plugin selector using `InquirerPy` with arrow-key navigation, Page-Up/Page-Down (20-entry jumps), and cursor position memory between sessions.
   - Non-blocking command loop using `msvcrt.kbhit()` (Windows) / `select.select()` (Unix) so the CLI detects GUI closure within 200ms without requiring an Enter press.
   - Live parameter display, preset browsing/loading, state save/restore commands.
   - Re-selection loop: after closing a plugin GUI, the user returns to the selector to open another.
5. **`generate_proto.py`**: Generates Python gRPC stubs and patches the broken absolute import (`import rps_pb2`) to a relative import (`from . import rps_pb2`).

Dependencies: `grpcio`, `grpcio-tools`, `rich`, `click`, `InquirerPy` (see `examples/python/pyproject.toml`).

---

## 4. Next Steps (Phase 7+)

The immediate next goals are:
1. **OS-Specific Formats**: Implement scanners for **AU** (macOS via CoreAudio) and **LV2** (Linux/Windows).
2. **Vendor SQLite**: Bundle the SQLite amalgamation to remove the system dependency.
3. **Extended Metadata**: Store additional plugin details (e.g., bus arrangements) if exposed by the SDKs.
4. **Audio Processing**: Add real-time audio streaming to hosted plugin GUIs via shared memory ring buffers.
5. **Plugin Chains**: Support hosting entire effects chains (e.g., EQ → Compressor) within a single worker process.
6. **Authentication**: Optional TLS/token auth for remote gRPC connections.
