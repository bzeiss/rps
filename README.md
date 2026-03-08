# RPS

RPS is a modern, cross-platform audio plugin scanner and host designed from the ground up for extreme robustness and reliability. It supports scanning VST2, VST3, CLAP, AU, AAX, LV2, and LADSPA formats on Windows, macOS, and Linux. Beyond scanning, RPS can host plugin GUIs in isolated processes and route audio through plugins via a lock-free shared memory transport.

RPS exposes a **gRPC API** so it can be driven from any language. Example clients in C++, Python and Java (possibly others) are included.

## Why RPS?

Scanning audio plugins is notoriously unreliable. Many plugins contain bugs, fail iLok license checks, or enter infinite loops during initialization. When a DAW or host application attempts to scan these plugins directly in its main process, a single bad plugin can crash the entire application.

RPS solves this by using a **multi-process architecture**:
- **`rps-server`**: A gRPC server that coordinates scanning and plugin GUI hosting. It manages a pool of worker processes, handles watchdogs/timeouts, streams progress events to clients, and aggregates results into a central SQLite database. If a plugin crashes, only the worker dies—the server logs the failure and moves on.
- **`rps-standalone`**: A standalone CLI wrapper around the same scan engine (no server needed).
- **`rps-pluginscanner`**: The worker. It isolates the unsafe, third-party plugin code from the rest of your system. Communicates with the engine via Protobuf over Boost.Interprocess message queues.
- **`rps-pluginhost-vst3`**: An isolated GUI host process for VST3 plugins. Opens the plugin's native editor in an SDL3 window with an ImGui preset sidebar. Communicates via Protobuf over stdin/stdout pipes. Supports real-time audio processing.
- **`rps-pluginhost-clap`**: An isolated GUI host process for CLAP plugins, using the same SDL3/ImGui sidebar architecture. Communicates via Protobuf over stdin/stdout pipes. Supports real-time audio processing.
- **`rps-audio`** (`libs/rps-audio/`): Shared memory SPSC lock-free ring buffer library for low-latency audio transport between processes.
- **`examples/`**: Client examples.

## Project Goals
1. **Primary Objective**: Robustness (Crash and stall isolation).
2. **Secondary Objective**: Performance / Speed (Parallel scanning).
3. **Third Objective**: Ease of use via central SQLite database (external tools can simply query the DB).
4. **Language**: Strictly Modern C++23 (core engine). Python for example clients.
5. **Dependencies**: STL, Boost, SQLite, gRPC/Protobuf, spdlog, and Plugin SDKs.

## Building

### Prerequisites

| Dependency   | Minimum Version | Notes                                              |
|--------------|-----------------|----------------------------------------------------|
| CMake        | 3.25            | Build system (4.2+ required for VS 2026)           |
| C++ Compiler | C++23 capable   | Clang 16+ (macOS+Linux), MSVC 2022 17.5+ (Windows) |
| Ninja        | 1.11+           | Build backend (Recommended)                        |
| vcpkg        | Latest          | C/C++ package manager (Used on ALL platforms)      |
| Git          | 2.x             | For version management                             |

All dependencies (`boost`, `grpc`, `protobuf`, `spdlog`, `sqlite3`) are managed automatically by `vcpkg` during the CMake configure step.

### Step 1: Clone the Repository

```bash
git clone https://github.com/bzeiss/rps.git
cd rps
git submodule update --init --recursive
```

The submodule init will download the plugin SDK headers (CLAP, VST3).

### Step 2: Configure and Build

RPS uses `vcpkg` across **all platforms** (Windows, macOS, and Linux) to ensure that the resulting executables are completely self-contained and statically linked. This prevents runtime crashes due to missing `.dll`, `.so`, or `.dylib` files.

1. **Install vcpkg** if you haven't already:
   Because we use `vcpkg` in manifest mode with specific dependency version overrides, vcpkg requires its repository to be checked out at the exact `builtin-baseline` specified in `vcpkg.json`.
   ```bash
   git clone https://github.com/microsoft/vcpkg.git /path/to/vcpkg
   cd /path/to/vcpkg
   # Checkout the specific baseline commit required by rps/vcpkg.json
   git checkout 62159a45e18f3a9ac0548628dcaf74fcb60c6ff9
   
   # Windows:
   bootstrap-vcpkg.bat
   # Linux/macOS:
   ./bootstrap-vcpkg.sh
   ```

2. **Install Ninja** (Windows):
   The Windows builds are configured to use the Ninja generator for significantly faster and more consistent compilation.
   ```cmd
   winget install Ninja-build.Ninja
   ```

#### Windows (MSVC)

```cmd
# Configure CMake to use vcpkg and static linking
# * leave out the VST2 parameters to build without it
# * use Ninja for faster builds, but it requires running from Developer Command Prompt
# * if using the MSBuild generator (e.g. -G "Visual Studio 18 2026"), CMake 4.2+ is required
# * adapt the paths

cmake -G Ninja -B build -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows-static -DRPS_MSVC_STATIC_RUNTIME=ON -DRPS_ENABLE_VST2=ON -DRPS_VST2_SDK_PATH=c:/dev/vstsdk2.4
      
# Build the project
cmake --build build --config Release
```

> **Important Note regarding process termination on Windows:**
> When using the example Java or C++ clients on Windows, it is recommended to run them from PowerShell rather than MSYS2/MinTTY terminals. MSYS2 terminals do not always translate `Ctrl+C` into proper Windows console control events, which can cause the client to abruptly terminate without running shutdown hooks, leaving the `rps-server.exe` running in the background as an orphaned process. Running from PowerShell or standard Command Prompt ensures proper process termination.

#### Windows (Clang MSVC ABI)

```cmd
# Enable VST2 with custom SDK path if needed: -DRPS_ENABLE_VST2=ON -DRPS_VST2_SDK_PATH=/path/to/vstsdk2.4
cmake -G Ninja -B build -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build build --config Release
```
#### Windows (Clang GNU/MinGW ABI in MSYS2 CLANG64)

```cmd
# Enable VST2 with custom SDK path if needed: -DRPS_ENABLE_VST2=ON -DRPS_VST2_SDK_PATH=/path/to/vstsdk2.4
cmake -G Ninja -B build -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-mingw-static
cmake --build build --config Release
```


#### macOS (Apple Silicon)

```bash
# Enable VST2 with custom SDK path if needed: -DRPS_ENABLE_VST2=ON -DRPS_VST2_SDK_PATH=/path/to/vstsdk2.4
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=arm64-osx-release
cmake --build build --config Release
```

#### Linux (Ubuntu/Debian/Fedora)

*Note: The default `x64-linux` triplet in vcpkg automatically builds static libraries.*
```bash
# Ensure you have build tools installed:
# Ubuntu: sudo apt install build-essential cmake ninja-build pkg-config curl zip unzip tar
# Fedora: sudo dnf install gcc-c++ cmake ninja-build pkgconf-pkg-config curl zip unzip tar

# Enable VST2 with custom SDK path if needed: -DRPS_ENABLE_VST2=ON -DRPS_VST2_SDK_PATH=/path/to/vstsdk2.4
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-linux
cmake --build build --config Release
```

#### Vcpkg Triplet Reference

| OS          | Architecture  | Compiler            | Recommended `VCPKG_TARGET_TRIPLET` | CMake Flags Required                                        |
|-------------|---------------|---------------------|------------------------------------|-------------------------------------------------------------|
| **Windows** | x64           | MSVC                | `x64-windows-static`               | `-DRPS_MSVC_STATIC_RUNTIME=ON`                              |
| **Windows** | x64           | Clang/MSVC ABI      | `x64-windows-static`               | `-DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl` |
| **Windows** | x64           | Clang/GNU/MinGW ABI | `x64-mingw-static`                 | `-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++`     |
| **Windows** | arm64         | MSVC ABI            | `x64-windows-static`               | `-DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl` |
| **macOS**   | arm64 (M1/M2) | Apple Clang         | `arm64-osx-release`                |                                                             |
| **macOS**   | x64 (Intel)   | Apple Clang         | `x64-osx-release`                  |                                                             |
| **Linux**   | x64           | GCC / Clang         | `x64-linux`                        |                                                             |
| **Linux**   | arm64         | GCC / Clang         | `arm64-linux`                      |                                                             |

---

### Build Output

After a successful build, you will find the following binaries in the `build/` directory:
- `build/apps/rps-standalone/rps-standalone` (or `.exe`) — standalone CLI
- `build/apps/rps-pluginscanner/rps-pluginscanner` (or `.exe`) — scanner worker
- `build/apps/rps-server/rps-server` (or `.exe`) — gRPC server
- `build/apps/rps-pluginhost-vst3/rps-pluginhost-vst3` (or `.exe`) — VST3 GUI host worker
- `build/apps/rps-pluginhost-clap/rps-pluginhost-clap` (or `.exe`) — CLAP GUI host worker
- `build/apps/rps-vstscannermaster/vstscannermaster` (or `.exe`) — Steinberg-compatible VST3 cache generator
- `build/examples/cpp/rps-example-client` (or `.exe`) — C++ gRPC example client

Visual Studio adds an additional layer of subdirectories "Release" or "Debug".

The dependant processes `rps-pluginscanner`, `rps-pluginhost-vst3`, and `rps-pluginhost-clap` will be copied automatically to the example directories.

## Usage

RPS can be used in five ways:
1. **Standalone CLI** (`rps-standalone`) — run scans directly from the command line.
2. **gRPC Server** (`rps-server`) — a long-lived daemon that accepts scan requests and plugin GUI hosting from any language client. See [gRPC Server](#grpc-server) and [Python TUI Client](#python-tui-client) below.
3. **Plugin GUI Hosting** (`rps-pluginhost-vst3`, `rps-pluginhost-clap`) — isolated worker processes that open a plugin's native GUI in an SDL3 window with an ImGui preset browser sidebar. Driven via gRPC through `rps-server`.
4. **Audio Processing** — send audio through hosted plugins via shared memory ring buffers. Use `open-gui --audio` in the Python client to enable audio, then `send-audio <file.wav>` to process a WAV file. For real-time playback through an audio device, use `open-gui --audio --audio-device sdl3`, then `play-audio <file.wav>` or `play-audio-looped <file.wav>` (shared memory path). For network-transparent gRPC playback, use `play-audio-grpc <file.wav>` or `play-audio-looped-grpc <file.wav>`.
5. **VST3 Scanner Master** (`vstscannermaster`) — drop-in replacement for Steinberg's `vstscannermaster.exe` that produces Cubase/Nuendo/Dorico-compatible XML cache files. See [VST3 Scanner Master](#vst3-scanner-master) below.

### Standalone CLI

The standalone CLI will automatically spawn the worker scanner processes as needed.

#### Command Line Arguments

```text
RPS Standalone Options:
  -h [ --help ]                         Produce help message
  -d [ --scan-dir ] arg                 Directories to recursively scan for plugins
  -s [ --scan ] arg                     Single file to scan
  -b [ --scanner-bin ] arg              Path to the scanner binary (default: rps-pluginscanner.exe)
  -t [ --timeout ] arg (=120000)        Timeout in ms per plugin (default: 2 min)
  -j [ --jobs ] arg (=6)                Number of parallel scanner workers (default: 6)
  -f [ --formats ] arg (=all)           Comma-separated list of formats to scan (e.g. vst3,clap) or 'all'
  -m [ --mode ] arg (=incremental)      Scan mode: 'full' or 'incremental' (see below)
  -r [ --retries ] arg (=3)             Number of retries for failed plugins (0 = no retries)
     --filter arg                       Only scan plugins whose filename contains this string
  -l [ --limit ] arg (=0)               Maximum number of plugins to scan (0 = unlimited)
  -v [ --verbose ]                      Enable verbose scanner output (plugin debug logs)
     --db arg (=rps-plugins.db)         Path to the output SQLite database file
```

### Examples

**1. Scan the default OS plugin directories for all formats**
If you run `rps-standalone` without any path arguments, it will automatically search the standard VST3, CLAP, AU, and AAX folders for your specific OS. VST2 is excluded from `all` and must be explicitly requested (see below).
```bash
./rps-standalone
```

**2. Scan only VST3 and CLAP formats**
```bash
./rps-standalone --formats vst3,clap
```

**3. Scan VST2 plugins (requires VST2 build — see [Optional: VST2.4 Support](#optional-vst24-support))**
```bash
# VST2 must be explicitly requested — it is never included in --formats all
./rps-standalone --formats vst2
./rps-standalone --formats vst3,clap,vst2
```

**4. Scan a specific directory**
```bash
# Windows
rps-standalone.exe --scan-dir "C:\Program Files\Common Files\VST3"

# macOS / Linux
./rps-standalone --scan-dir "/Library/Audio/Plug-Ins/VST3"
```

**5. Filter plugins by name and limit the count (useful for debugging)**
```bash
rps-standalone.exe --formats vst3 --filter "FabFilter" --limit 10
```

**6. Scan multiple directories with a specific number of workers**
```bash
rps-standalone.exe --scan-dir "C:\Folder1" "D:\Folder2" --jobs 4
```

**7. Scan a single plugin with verbose debug output**
```bash
rps-standalone.exe --scan "C:\VstPlugins\Massive.dll" --verbose
```

**8. Scan with a longer timeout (for slow iLok-protected plugins)**
```bash
rps-standalone.exe --timeout 180000
```

**9. Force a full rescan (clear and rescan all plugins of the requested formats)**
```bash
./rps-standalone --mode full
./rps-standalone --formats vst2 --mode full
```

**10. Incremental scan (default -- skip unchanged plugins)**
```bash
# This is the default. Only new or modified plugins are scanned.
./rps-standalone
```

### Default Plugin Paths

If no paths are provided, RPS will automatically search the following directories based on your operating system (32bit plugins are not supported!):

**Windows:**
- `C:\Program Files\Common Files\VST3`
- `C:\Program Files\Common Files\CLAP`
- `C:\Program Files\Common Files\Avid\Audio\Plug-Ins` (AAX)
- `C:\Program Files\Steinberg\VstPlugins` (VST2)
- `C:\Program Files\VstPlugins` (VST2)

**macOS:**
- `/Library/Audio/Plug-Ins/VST3` (and `~/Library/...`)
- `/Library/Audio/Plug-Ins/CLAP` (and `~/Library/...`)
- `/Library/Audio/Plug-Ins/Components` (AU) (and `~/Library/...`)
- `/Library/Application Support/Avid/Audio/Plug-Ins` (AAX)
- `/Library/Audio/Plug-Ins/VST` (VST2) (and `~/Library/...`)

**Linux:**
- `/usr/lib/vst3`, `/usr/local/lib/vst3`, `~/.vst3`
- `/usr/lib/clap`, `/usr/local/lib/clap`, `~/.clap`
- `/usr/lib/lv2`, `/usr/local/lib/lv2`, `~/.lv2`
- `/usr/lib/vst`, `/usr/local/lib/vst`, `~/.vst`
- `/usr/lib/ladspa`, `/usr/lib64/ladspa`, `/usr/local/lib/ladspa`, `/usr/local/lib64/ladspa`, `~/.ladspa`

## Scan Modes

RPS supports two scan modes via `--mode`:

- **`incremental`** (default): Only scans plugins that are new or have changed since the last scan. Change detection uses file modification time (`mtime`) comparison. Plugins that no longer exist on disk are automatically pruned. Previously **skipped** plugins (e.g., empty bundles) and **blocked** plugins (exhausted all retries) are also remembered and not re-scanned unless their file changes. This makes repeated scans near-instant.

- **`full`**: Clears all database entries **for the requested format(s)** and rescans everything. A `--mode full --formats vst2` will only clear VST2 entries, leaving CLAP and VST3 results intact. This also clears the skipped and blocked lists for the requested formats.

## Database Schema

Scan results are stored in a SQLite database (default: `rps-plugins.db`) with WAL journal mode for performance.

| Table | Purpose |
|---|---|
| `plugins` | One row per successfully scanned plugin: `format`, `path`, `name`, `uid`, `vendor`, `version`, `description`, `url`, `category`, `num_inputs`, `num_outputs`, `status`, `error_message`, `scan_time_ms`, `file_mtime`, `file_hash`, `last_scanned` |
| `parameters` | Plugin parameters linked by `plugin_id`: `param_index`, `name`, `default_value` |
| `aax_plugins` | AAX-specific variant data (1:N with `plugins`): manufacturer/product/plugin IDs (FourCC + numeric), effect ID, plugin type, stem formats (input/output/sidechain) |
| `plugins_skipped` | Plugins that are not scannable (e.g., empty bundles, no loadable binary): `path`, `format`, `reason`, `file_mtime` |
| `plugins_blocked` | Plugins that exhausted all retries or timed out: `path`, `format`, `reason`, `file_mtime` |
| `vst3_classes` | VST3 multi-class entries (1:N with `plugins`): `class_index`, `name`, `uid`, `category`, `class_category`, `cardinality`, `class_flags`, `sub_categories`, `sdk_version`, `vendor`, `version` |
| `vst3_compat_uids` | VST3 compatibility UIDs (1:N with `vst3_classes`): `class_id`, `new_uid`, `old_uid` |

## VST3 Scanner Master

`vstscannermaster` is an experimental drop-in replacement for Steinberg's `vstscannermaster.exe`. It uses the rps crash-isolated scanning infrastructure to produce the same XML cache files that Cubase, Nuendo, and Dorico consume. It's currently not working properly in Cubase.

### Command Line Arguments

```text
vstscannermaster -prefPath <dir> [options]

  -prefPath <dir>       Cache output directory (required)
  -licenceLevel <N>     Licence level (accepted but ignored)
  -hostName <name>      Host name
  -progress             Show progress output
  -rescan               Force full rescan (default: incremental)
  -timeout <secs>       Per-plugin timeout in seconds (default: 120)
  -recheckPath <path>   Rescan a single plugin path
  --scanner-bin <path>  Path to the scanner binary
  -j [ --jobs ] <N>     Parallel scanner workers (default: 6)
  -v [ --verbose ]      Verbose output
```

### Example

```bash
vstscannermaster -prefPath ./VST3Cache -licenceLevel 25000 -progress -rescan
```

### Output Files

| File                   | Description                                                           |
|------------------------|-----------------------------------------------------------------------|
| `vst3plugins.xml`      | All successfully scanned VST3 plugins with full class metadata        |
| `vst3blocklist.xml`    | Plugins that crashed or timed out during scanning                     |
| `vst3allowlist.xml`    | User-managed allow list (created empty, preserved on subsequent runs) |
| `cacheVersion`         | Cache format version                                                  |
| `vstscannermaster.log` | Scan log with summary statistics                                      |
| `rps-cache.db`         | Internal SQLite database (used for incremental scanning)              |

## gRPC Server

`rps-server` exposes the scan engine as a gRPC service, making RPS usable from any language.

### Server CLI

```text
RPS Server Options:
  -h [ --help ]                    Produce help message
  -p [ --port ] arg (=50051)       gRPC listen port
     --db arg (=rps-plugins.db)    Path to the SQLite database file
  -b [ --scanner-bin ] arg         Path to the scanner binary
```

### gRPC API

Defined in `proto/rps.proto`:

| RPC              | Type             | Description                                                             |
|------------------|------------------|-------------------------------------------------------------------------|
| `StartScan`      | Server streaming | Start a scan, returns a stream of `ScanEvent` messages until completion |
| `StopScan`       | Unary            | Stop a running scan gracefully                                          |
| `GetStatus`      | Unary            | Query server state (idle/scanning), uptime, db path                     |
| `ListPlugins`    | Unary            | List all scanned plugins from the database                              |
| `OpenPluginGui`  | Server streaming | Open a plugin GUI, returns `PluginEvent` stream (params, presets, audio) |
| `ClosePluginGui` | Unary            | Close a running plugin GUI                                              |
| `GetPluginState` | Unary            | Save the complete state of a running plugin as a binary blob            |
| `SetPluginState` | Unary            | Restore a plugin's state from a previously saved blob                   |
| `LoadPreset`     | Unary            | Load a preset by ID on a running plugin GUI                             |
| `StreamAudio`    | Bidi streaming   | Stream audio blocks through a hosted plugin via gRPC (networked)        |
| `Shutdown`       | Unary            | Graceful server shutdown                                                |

`OpenPluginGui` accepts optional audio parameters (`enable_audio`, `sample_rate`, `block_size`, `num_channels`). When audio is enabled, the stream emits an `AudioReady` event containing the shared memory segment name for the ring buffer.

`StreamAudio` is a bidirectional streaming RPC for network-transparent audio processing. The client sends `AudioInputBlock` messages (with raw float32 audio data), and the server returns processed `AudioOutputBlock` messages. Requires an active GUI session with audio enabled. Adds gRPC network latency compared to the local shared memory path.

Only one scan at a time is allowed. `StartScan` returns `ALREADY_EXISTS` if a scan is in progress.

### Starting the Server

```bash
# Start with defaults (port 50051, rps-plugins.db)
./rps-server

# Custom options
./rps-server --port 50051 --db my-plugins.db --log-level debug
```

The gRPC clients spawn the server automatically.

## Example Clients

Each example client has its own README.md in its directory.

### Python TUI Client — Audio Processing

The Python client supports sending audio through plugins via shared memory or gRPC:

```bash
cd examples/python

# Open a plugin with audio processing enabled (file-based: writes output WAV)
uv run python -m rps_client open-gui --format vst3 --audio

# Inside the interactive session:
#   send-audio test.wav          — process via shared memory (low latency)
#   send-audio-grpc test.wav     — process via gRPC stream (networked)
# Output is written to test_processed.wav

# Open a plugin with real-time audio device playback
uv run python -m rps_client open-gui --format vst3 --audio --audio-device sdl3 -bs 512

# Inside the interactive session:
#   play-audio test.wav          — real-time playback via shared memory
#   play-audio-looped test.wav   — looped playback via shared memory
#   play-audio-grpc test.wav     — real-time playback via gRPC
#   play-audio-looped-grpc test.wav — looped playback via gRPC
```

Audio options for `open-gui`:
| Option | Default | Description |
|---|---|---|
| `--audio` | off | Enable shared memory audio transport |
| `--audio-device` | none | Audio output device (`sdl3` for real-time playback) |
| `--sample-rate` / `-sr` | 48000 | Audio sample rate (match your WAV) |
| `--channels` / `-ch` | 2 | Channel count |
| `--block-size` / `-bs` | 128 | Processing block size in samples |

## Logging Configuration

All RPS processes use **environment variables** for logging control. No CLI flags are needed — set the variables before launching any RPS tool, and child processes inherit them automatically.

| Variable | Default | Description |
|---|---|---|
| `RPS_SERVER_LOGGING` | `false` | Enable file logging for `rps-server` |
| `RPS_SERVER_LOGLEVEL` | `info` | Log level: `trace`, `debug`, `info`, `warn`, `error` |
| `RPS_PLUGINSCANNER_LOGGING` | `false` | Enable file logging for scanner workers |
| `RPS_PLUGINSCANNER_LOGLEVEL` | `info` | Log level for scanner workers |
| `RPS_PLUGINHOST_LOGGING` | `false` | Enable file logging for plugin host workers |
| `RPS_PLUGINHOST_LOGLEVEL` | `info` | Log level for plugin host workers |
| `RPS_LOG_DIR` | *(auto)* | Directory for log files. Set automatically by `rps-server` to its CWD so child processes (scanner, pluginhost) write logs alongside the server. Override to redirect all logs elsewhere. |

### Log File Names

All log files are written to the directory specified by `RPS_LOG_DIR`. When launched via `rps-server`, this is automatically set to the server's working directory, so all logs appear alongside `rps-server.log`.

| Process | Log File |
|---|---|
| `rps-server` | `rps-server.log` |
| `rps-pluginscanner` | `rps-pluginscanner.worker_{N}.log` (one per worker) |
| `rps-pluginhost` | `rps-pluginhost.{format}.{plugin_name}.log` |

Log messages at `info` level and above are flushed immediately; `debug`/`trace` messages are flushed every 3 seconds.

### Example: Diagnosing scanner timeouts

```bash
# Windows (PowerShell / cmd)
set RPS_PLUGINSCANNER_LOGGING=true
set RPS_PLUGINSCANNER_LOGLEVEL=debug
rps-standalone --formats vst3

# After the scan, check the per-worker log files to see where a plugin hung
```

## Documentation

For developers looking to contribute, understand the architecture, or build the project from source, please read the [Developer Guide](DEVELOPMENT.md).

## License

MIT License
