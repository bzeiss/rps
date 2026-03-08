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
   - It connects to the engine via IPC (Boost.Interprocess message queues with Protobuf serialization), receives a `ScanCommand`, and loads a single target plugin into its own memory space.
   - It extracts the necessary metadata (name, parameters, inputs/outputs) and reports it back via `ScanProgressEvent` and `ScanResultEvent` messages.

6. **`rps-pluginhost-vst3`** (VST3 GUI Host Worker)
   - An isolated process that loads a VST3 plugin, creates an SDL3 window, and embeds the plugin's native editor view.
   - Features an ImGui preset browser sidebar (collapsible, searchable, multi-column table).
   - Discovers presets from `IUnitInfo` (`IProgramListData`) and `.vstpreset` file scanning across standard directories (`%APPDATA%/VST3 Presets/`, `Documents/VST3 Presets/`, `%COMMONPROGRAMFILES%/VST3 Presets/`) with vendor-agnostic matching.
   - Loads presets via `IUnitInfo` + minimal audio process pass, or `.vstpreset` file parsing (inline binary parser for component/controller state chunks).
   - Communicates with the server via length-prefixed Protobuf over stdin/stdout pipes.

7. **`rps-pluginhost-clap`** (CLAP GUI Host Worker)
   - An isolated process that loads a CLAP plugin, creates an SDL3 window, and embeds the plugin's native editor view.
   - Same ImGui preset sidebar architecture as the VST3 host.
   - Discovers and loads presets via the CLAP preset-load extension.
   - Communicates with the server via length-prefixed Protobuf over stdin/stdout pipes.

---

## 2. Technology Stack & Constraints

We adhere to a set of strict constraints to ensure maximum compatibility, adoption, and reliability in the audio industry:

- **Language**: Strictly **Modern C++23**. (C++ is the undisputed standard in audio programming).
- **Build System**: **CMake** (v3.25+).
- **Compilers**: The project must build cleanly with **Clang**, **GCC**, and **MSVC**.
- **Allowed Dependencies**:
  - The C++ Standard Template Library (STL).
  - **Boost 1.90** (specifically `Boost.Process`, `Boost.Interprocess`, `Boost.JSON` (scanner only, for VST3 moduleinfo.json parsing), `Boost.Program_options`, `Boost.Filesystem`, `Boost.UUID`).
  - **SQLite3** (for the final output database).
  - **gRPC / Protobuf** (for the `rps-server` API layer).
  - **spdlog** (structured logging across all processes — server, scanner, pluginhost — controlled via environment variables).
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

## 3. Current Implementation Status (Phases 1-7)

We have successfully completed **Phase 1 (IPC Foundation)**, **Phase 2 (Process Pool & Orchestration)**, **Phase 3 (Format Scanners)**, **Phase 4 (Incremental Scanning & Database Optimization)**, **Phase 5 (AAX Scanner & Robustness)**, **Phase 6 (gRPC Server & Python TUI Client)**, and **Phase 7 (Shared Memory Audio Processing)**. The full stack — from IPC to gRPC streaming to a rich Python TUI with audio processing — is working end-to-end and tested against thousands of real-world plugins.

### 3.1 Project Structure

#### Libraries
- `libs/rps-core/`: Format traits, registry, and filesystem discovery logic.
- `libs/rps-ipc/`: Shared IPC transport layer (engine ↔ scanner/host). Provides two transport implementations:
  - `Connection.hpp/.cpp` (`MessageQueueConnection`): Boost.Interprocess message queues with Protobuf serialization. Used by the scanner path.
  - `StdioPipeConnection.hpp/.cpp`: Length-prefixed Protobuf over stdin/stdout pipes. Used by the pluginhost path.
  - `Messages.hpp`: Shared C++ data structs (`ScanResult`, `ParameterInfo`) used internally by the engine.
- `libs/rps-engine/`: **The reusable scan engine library** (namespace `rps::engine`). Contains all orchestration logic extracted from the original CLI app:
  - `ScanEngine.hpp/.cpp`: Top-level `ScanEngine` class with `runScan(ScanConfig, ScanObserver*)`. Thread-safe (one scan at a time).
  - `ScanConfig`: Struct encapsulating all scan parameters (formats, mode, jobs, timeout, etc.).
  - `ProcessPool.hpp/.cpp`: Worker process lifecycle, IPC message loop, retry logic, watchdog.
  - `ScanObserver.hpp`: Abstract observer interface for scan progress events.
  - `ConsoleScanObserver.hpp/.cpp`: Console-based observer for CLI use.
  - `db/DatabaseManager.hpp/.cpp`: SQLite database layer — schema, upserts, incremental cache, skipped/blocked management.
- `libs/rps-gui/`: **Shared GUI hosting library** used by both plugin host processes:
  - `IPluginGuiHost.hpp`: Abstract interface for format-specific GUI hosts (open, event loop, params, state, presets, audio processing).
  - `SdlWindow.hpp/.cpp`: SDL3 window management with Dear ImGui integration. Implements the collapsible preset browser sidebar (search filter, multi-column sortable table), "Stationary Content" window expansion pattern, and child HWND positioning to solve the Windows Airspace Problem.
  - `GuiWorkerMain.hpp/.cpp`: Common IPC event loop for host worker processes. Spawns a dedicated real-time audio thread (AVRT Pro Audio priority on Windows) when `--audio-shm` is present.
- `libs/rps-audio/`: **Shared memory audio transport library**:
  - `SharedAudioRing.hpp/.cpp`: SPSC (Single Producer, Single Consumer) lock-free ring buffer over shared memory. Uses `boost::interprocess::windows_shared_memory` on Windows (pagefile-backed named kernel sections) and `boost::interprocess::shared_memory_object` on POSIX. Header contains audio format config, cache-line-aligned atomic positions, and extension points for future DAW features (sidechain, sends, transport state).

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
   - `src/GuiSessionManager.cpp`: Manages active plugin GUI sessions. Spawns the appropriate host worker process (`rps-pluginhost-vst3` or `rps-pluginhost-clap`) with stdin/stdout pipes, establishes IPC via `StdioPipeConnection`, creates shared memory audio ring buffers when audio is requested, and relays events (including `AudioReady`) between the gRPC stream and the host worker.
  - Uses `spdlog` for dual-sink logging (stdout + file).

- `apps/rps-pluginhost-vst3/`: **VST3 GUI host worker** process.
  - `src/Vst3GuiHost.cpp`: Implements `IPluginGuiHost` for VST3 plugins. Handles COM module loading, `IComponent`/`IEditController` setup, `IPlugView` embedding in the SDL3 window, parameter discovery, preset discovery (via `IUnitInfo` and `.vstpreset` file scanning), preset loading, and **audio processing** (multi-bus negotiation with sidechain support via `activateBus`/`setBusArrangements`, de-interleaving/re-interleaving, `IAudioProcessor` calls with proper `ProcessContext` transport state, `ParameterChanges`, and `ConnectionProxy` message forwarding between component and controller for FabFilter-style real-time metering).

- `apps/rps-pluginhost-clap/`: **CLAP GUI host worker** process.
  - `src/ClapGuiHost.cpp`: Implements `IPluginGuiHost` for CLAP plugins. Handles CLAP plugin loading, GUI embedding, parameter discovery, preset discovery/loading via the CLAP preset-load extension, host extension stubs (`clap.params`, `clap.latency`, `clap.state`) to prevent crashes in plugins that cache extension pointers, and **audio processing** (multi-port audio with sidechain support, activation, processing via `clap_plugin.process()`).

#### Proto
- `proto/rps.proto`: gRPC service and message definitions. `ScanEvent` is a `oneof` union mirroring all `ScanObserver` callbacks 1:1. `PluginEvent` is a `oneof` union for GUI hosting events (gui_opened, gui_closed, parameter_list, parameter_updates, preset_list, preset_loaded, audio_ready, gui_error). `OpenPluginGuiRequest` includes optional audio parameters (`enable_audio`, `sample_rate`, `block_size`, `num_channels`). `StreamAudio` is a bidirectional streaming RPC that proxies `AudioInputBlock`/`AudioOutputBlock` messages through the server’s shared memory ring for network-transparent audio processing.

#### Example Clients
- `examples/python/`: Python TUI client using `rich` for per-worker progress bars. Features an interactive `open-gui` command with an `InquirerPy` fuzzy plugin selector (arrow keys, Page-Up/Down for 20-entry jumps, cursor position memory). Supports audio processing via `open-gui --audio` with two paths: `send-audio <file.wav>` (local shared memory) and `send-audio-grpc <file.wav>` (bidirectional gRPC streaming). Real-time audio device playback via `--audio-device sdl3` with `play-audio <file.wav>` and `play-audio-looped <file.wav>` (looped playback, Enter/Esc to stop). Spawns/kills `rps-server` as a subprocess.
- `examples/cpp/`: C++ gRPC client with ANSI terminal TUI. Same features as the Python client: auto-spawns server, per-worker progress bars, streaming results.

### 3.2 The IPC Protocol

RPS uses **Protobuf** for all internal IPC serialization. The protocol definitions are split across two `.proto` files:

#### Scanner IPC (`proto/scanner.proto`)
Used between the scan engine (`ProcessPool`) and `rps-pluginscanner`:
- `ScanCommand`: Sent by Engine → Scanner. Contains the path to the plugin and the target format.
- `ScannerEvent`: Sent by Scanner → Engine. A `oneof` union containing:
  - `ScanResultEvent`: On success — rich DAW-ready metadata: Name, Vendor, Version, UID, Description, URL, Categories, Parameter lists, and an `extra_data` map for format-specific metadata (e.g., AAX variant triad IDs).
  - `ScanProgressEvent`: Percentage and status strings (e.g., "Instantiating Plugin"). Crucial for resetting the engine's watchdog timer.
  - `ScanErrorEvent`: On caught, non-fatal errors. Errors prefixed with `SKIP:` are treated as non-scannable plugins (stored in `plugins_skipped`).

#### Pluginhost IPC (`proto/host.proto`)
Used between `rps-server` and the GUI host workers (`rps-pluginhost-vst3`, `rps-pluginhost-clap`):
- `HostCommand`: Commands from server → host (open GUI, close GUI, get params, load preset, save/restore state, etc.).
- `HostEvent`: Events from host → server (GUI opened/closed, parameter updates, preset lists, audio ready, errors).

Protobuf messages are converted to/from internal C++ structs (`rps::ipc::ScanResult`, `rps::ipc::ParameterInfo`) at the IPC boundary. The rest of the codebase (DatabaseManager, ScanObserver, etc.) uses the C++ structs directly.

### 3.3 The IPC Transport Layer
Located in `libs/rps-ipc/`.

RPS uses two transport mechanisms, chosen to best fit each communication pattern:

#### Scanner Path: Boost.Interprocess Message Queues
The scanner path (`ProcessPool` ↔ `rps-pluginscanner`) uses **Boost.Interprocess** `message_queue`:
- Each worker is assigned a unique UUID by the engine at startup. The engine spawns two queues per worker (one for TX, one for RX) named using that UUID.
- The scanner receives this UUID via command-line arguments and connects to the existing queues.
- **Rationale**: Message queues are well-suited for the scanner's crash-prone workflow. Stdout/stderr from the scanner process are captured separately for crash diagnostics, and the MQ transport keeps the IPC channel independent of the process's standard I/O streams.

#### Pluginhost Path: stdin/stdout Pipes
The pluginhost path (`GuiSessionManager` ↔ `rps-pluginhost-*`) uses **length-prefixed Protobuf over stdin/stdout pipes** (`StdioPipeConnection`):
- Wire format: `[4-byte uint32_t length, little-endian][N-byte serialized protobuf payload]`.
- The server spawns the host process with `boost::process` pipe redirection (`bp::std_in < cmdPipe`, `bp::std_out > evtPipe`).
- **Plugin stdout protection**: At startup, before any plugin code runs, the host process saves the original stdout fd, then redirects stdout → stderr via `_dup2(2, 1)` (Windows) / `dup2(STDERR_FILENO, STDOUT_FILENO)` (POSIX). The saved original stdout fd is used for the IPC pipe. Any plugin stdout output goes harmlessly to stderr/logs.
- OS-native handles are used directly for I/O with reliable timeout support (`PeekNamedPipe`/`ReadFile`/`WriteFile` on Windows, `poll`/`read`/`write` on POSIX).
- **Rationale**: Pipes are simpler, eliminate named shared memory, and enable proper capture of plugin stdout without corrupting the IPC stream. No `--ipc-id` argument is needed.

### 3.4 Crash Isolation, Watchdog Logic, and UI Suppression
The core feature of RPS is fully implemented in `ProcessPool.cpp` and `scanner/main.cpp`:

1. The engine uses `boost::process` to spawn the scanner and passes the IPC UUID (for the scanner path) or uses pipe redirection (for the pluginhost path).
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

### 3.12 Logging System

All RPS processes use a unified logging system based on **spdlog**, controlled entirely via **environment variables** (`RPS_{PREFIX}_LOGGING` and `RPS_{PREFIX}_LOGLEVEL`). No CLI flags are used for logging.

The shared helper `rps::core::initLogging()` in `libs/rps-core/src/LoggingInit.cpp`:
- Reads `RPS_{PREFIX}_LOGGING` and `RPS_{PREFIX}_LOGLEVEL` from the environment.
- Reads `RPS_LOG_DIR` to determine where log files are created (falls back to CWD).
- When enabled: creates spdlog with stderr + file sinks at the requested level.
- When disabled (default): sets spdlog level to `off` (all calls become near-zero-cost no-ops).
- `flush_on(info)`: all info-level and above messages are flushed immediately (critical for short-lived processes like the scanner).
- `flush_every(3s)`: debug/trace messages are flushed periodically.

Log file location:
- **`rps-server`** sets `RPS_LOG_DIR` to its own CWD at startup (via `_putenv_s`/`setenv`). This is inherited by child processes (scanner, pluginhost), which run in temp directories but write their logs alongside the server.

Log file naming:
- **`rps-server`**: `rps-server.log`
- **`rps-pluginscanner`**: `rps-pluginscanner.worker_{N}.log` (worker ID passed via `--worker-id` from ProcessPool).
- **`rps-pluginhost`**: `rps-pluginhost.{format}.{plugin_name}.log` (constructed from CLI args).

The scanner's `g_verbose` global (used by individual scanner implementations like `Vst3Scanner.cpp` etc.) is driven by the effective spdlog level: `g_verbose = true` when the level is `debug` or `trace`.

---

### 3.13 gRPC Server Architecture

The `rps-server` application exposes the `rps::engine::ScanEngine` via a gRPC service defined in `proto/rps.proto`. The key insight is that the existing `ScanObserver` abstract interface is a natural event bus — `GrpcScanObserver` implements it and serializes each callback into a protobuf `ScanEvent` message written to the gRPC response stream.

Architecture:
```
Client (Python/C++/etc.)  ──gRPC──▶  RpsServiceImpl  ──▶  ScanEngine  ──▶  ProcessPool  ──IPC──▶  rps-pluginscanner
         ◀── stream ScanEvent ──          GrpcScanObserver
```

- **One scan at a time**: `ScanEngine::m_scanning` atomic flag. Returns `ALREADY_EXISTS` gRPC status if busy.
- **Logging**: `spdlog` with stderr + file sinks, controlled via `RPS_SERVER_LOGGING` / `RPS_SERVER_LOGLEVEL` environment variables (see §3.12).
- **Shutdown**: `Shutdown` RPC triggers `grpc::Server::Shutdown()` asynchronously (after returning the response). Signal handlers (SIGINT/SIGTERM) also trigger graceful shutdown.
- **Lifecycle**: Designed to be spawned and killed by a parent application. The Python example client demonstrates this pattern.

---

### 3.14 Python TUI Client

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
   - **`--audio` flag**: When enabled, opens the plugin with a shared memory ring buffer. Two audio processing paths:
     - `send-audio <file.wav>`: Writes blocks directly to the SPSC ring buffer (minimal latency, local only).
     - `send-audio-grpc <file.wav>`: Streams blocks via the `StreamAudio` bidirectional gRPC RPC (network-transparent, adds gRPC roundtrip latency). The server proxies audio to/from the same ring buffer.
   - Both paths collect processed output and write it to `<name>_processed.wav`.
   - `rps_audio.py`: Pure-Python shared memory client using `ctypes` kernel32 APIs (Windows) or `/dev/shm` (POSIX) to access the lock-free ring buffer. No external dependencies.
   - Re-selection loop: after closing a plugin GUI, the user returns to the selector to open another.
5. **`generate_proto.py`**: Generates Python gRPC stubs and patches the broken absolute import (`import rps_pb2`) to a relative import (`from . import rps_pb2`).

Dependencies: `grpcio`, `grpcio-tools`, `rich`, `click`, `InquirerPy` (see `examples/python/pyproject.toml`).

---

## 4. Next Steps (Phase 8+)

The immediate next goals are:
1. **OS-Specific Formats**: Implement scanners for **AU** (macOS via CoreAudio) and **LV2** (Linux/Windows).
2. **Vendor SQLite**: Bundle the SQLite amalgamation to remove the system dependency.
3. **Extended Metadata**: Store additional plugin details (e.g., bus arrangements) if exposed by the SDKs.
4. **Multi-Channel Audio**: Extend the shared memory ring buffer to support flexible bus layouts (mono/stereo/surround/Atmos), per-channel routing, and bus layout negotiation.
5. **Plugin Chains**: Support hosting entire effects chains (e.g., EQ → Compressor) within a single worker process, with chain splitters for parallel processing and per-plugin routing.
6. **Delay Compensation**: Implement plugin delay compensation (PDC) across chains using reported latency values.
7. **Authentication**: Optional TLS/token auth for remote gRPC connections.
