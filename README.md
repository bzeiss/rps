# RPS - Reliable Plugin Scanner

RPS is a modern, cross-platform audio plugin scanner designed from the ground up for extreme robustness and reliability. It supports scanning VST2, VST3, CLAP, AAX, AU, and LV2 formats on Windows, macOS, and Linux.

RPS exposes a **gRPC API** so it can be driven from any language. A Python TUI example client is included.

## Why RPS?

Scanning audio plugins is notoriously unreliable. Many plugins contain bugs, fail iLok license checks, or enter infinite loops during initialization. When a DAW or host application attempts to scan these plugins directly in its main process, a single bad plugin can crash the entire application.

RPS solves this by using a **multi-process architecture**:
- **`rps-server`**: A gRPC server that coordinates scanning. It manages a pool of worker processes, handles watchdogs/timeouts, streams progress events to clients, and aggregates results into a central SQLite database. If a plugin crashes, only the worker dies—the server logs the failure and moves on.
- **`rps-standalone`**: A standalone CLI wrapper around the same scan engine (no server needed).
- **`rps-pluginscanner`**: The worker. It isolates the unsafe, third-party plugin code from the rest of your system.
- **`examples/python/`**: A Python TUI client using `rich` that spawns/kills the server and displays per-worker progress bars.

## Project Goals
1. **Primary Objective**: Robustness (Crash and stall isolation).
2. **Secondary Objective**: Performance / Speed (Parallel scanning).
3. **Third Objective**: Ease of use via central SQLite database (external tools can simply query the DB).
4. **Language**: Strictly Modern C++23 (core engine). Python for example clients.
5. **Dependencies**: STL, Boost, SQLite, gRPC/Protobuf, spdlog, and Plugin SDKs.

## Building

### Prerequisites

| Dependency | Minimum Version | Notes |
|---|---|---|
| CMake | 3.25 | Build system |
| C++ Compiler | C++23 capable | Clang 16+ (recommended), GCC 13+, MSVC 2022 17.5+ |
| Ninja | 1.11+ | Recommended build backend (optional, can use Make or VS) |
| SQLite3 | 3.x | For the plugin database |
| gRPC | 1.60+ | For the `rps-server` gRPC API |
| spdlog | 1.12+ | Structured logging for `rps-server` |
| Git | 2.x | For cloning submodules |

**Boost 1.90** is built from source — no system Boost installation is needed. You must provide a path to a Boost source tree (see Step 1).

### Step 1: Clone and Set Up Dependencies

```bash
git clone <your-repo-url>
cd rps
git submodule update --init --recursive
```

The submodule init will download the plugin SDK headers (CLAP, VST3).

#### Boost Source Tree

RPS requires the **Boost 1.90 source tree** cloned from GitHub. The official boost.org tarball does **not** include CMake support and will not work.

```bash
git clone https://github.com/boostorg/boost.git /path/to/boost
cd /path/to/boost
git checkout boost-1.90.0
git submodule update --init --recursive
```

This will take several minutes (~180 sub-repos). Once done, the directory will contain a `CMakeLists.txt` at the top level.

Then tell CMake where it is using either method:

```bash
# Via CMake flag:
cmake -G Ninja -DBOOST_SOURCE_DIR=/path/to/boost -B build
cmake --build build

# Via environment variable:
export BOOST_SOURCE_DIR=/path/to/boost
cmake -G Ninja -B build
cmake --build build
```

The `-D` flag takes priority over the environment variable. If neither is set, CMake looks for `third_party/boost` as a fallback.

### Step 2: Configure and Build

#### Windows (MSYS2 / Clang) — Recommended

Open the **MSYS2 CLANG64** shell and install build tools:
```bash
pacman -S mingw-w64-clang-x86_64-cmake mingw-w64-clang-x86_64-ninja \
          mingw-w64-clang-x86_64-clang mingw-w64-clang-x86_64-sqlite3 \
          mingw-w64-clang-x86_64-grpc mingw-w64-clang-x86_64-spdlog
```

Configure and build (see [Boost Source Tree](#boost-source-tree) for how to obtain Boost):
```bash
# Option 1: Pass Boost path via CMake flag
cmake -G Ninja -DBOOST_SOURCE_DIR=/c/develop/boost -B build
cmake --build build

# Option 2: Set as environment variable
export BOOST_SOURCE_DIR=/c/develop/boost
cmake -G Ninja -B build
cmake --build build
```

#### Windows (MSVC / Visual Studio 2022)

Install SQLite3 via [vcpkg](https://vcpkg.io) or manually, then:
```bash
# Option 1: Pass Boost path via CMake flag
cmake -G "Visual Studio 17 2022" -A x64 -DBOOST_SOURCE_DIR=C:/develop/boost -B build
cmake --build build --config Release

# Option 2: Set as environment variable
set BOOST_SOURCE_DIR=C:\develop\boost
cmake -G "Visual Studio 17 2022" -A x64 -B build
cmake --build build --config Release
```

#### macOS (Homebrew)

Install build tools via Homebrew:
```bash
brew install cmake ninja sqlite pkg-config grpc protobuf spdlog
```

Configure and build:
```bash
# Option 1: Pass Boost path via CMake flag
cmake -G Ninja -DBOOST_SOURCE_DIR=/usr/local/src/boost -B build
cmake --build build

# Option 2: Set as environment variable
export BOOST_SOURCE_DIR=/usr/local/src/boost
cmake -G Ninja -B build
cmake --build build
```

#### Linux (Fedora)

```bash
sudo dnf install cmake ninja-build gcc-c++ clang sqlite-devel git grpc-devel grpc-plugins spdlog-devel
```

Configure and build:
```bash
# Option 1: Pass Boost path via CMake flag
cmake -G Ninja -DBOOST_SOURCE_DIR=/home/user/boost -B build
cmake --build build

# Option 2: Set as environment variable
export BOOST_SOURCE_DIR=/home/user/boost
cmake -G Ninja -B build
cmake --build build
```

#### Linux (Ubuntu / Debian)

```bash
sudo apt install cmake ninja-build g++ clang libsqlite3-dev git \
     libgrpc++-dev protobuf-compiler-grpc libspdlog-dev
```

Configure and build:
```bash
# Option 1: Pass Boost path via CMake flag
cmake -G Ninja -DBOOST_SOURCE_DIR=/home/user/boost -B build
cmake --build build

# Option 2: Set as environment variable
export BOOST_SOURCE_DIR=/home/user/boost
cmake -G Ninja -B build
cmake --build build
```

> **Note:** Boost is no longer needed as a system package on any platform. It is built from source as part of the CMake configure step.

### Build Output

After a successful build, you will find five binaries in the `build/` directory:
- `build/apps/rps-standalone/rps-standalone` (or `.exe`) — standalone CLI
- `build/apps/rps-pluginscanner/rps-pluginscanner` (or `.exe`) — scanner worker
- `build/apps/rps-server/rps-server` (or `.exe`) — gRPC server
- `build/apps/rps-vstscannermaster/vstscannermaster` (or `.exe`) — Steinberg-compatible VST3 cache generator
- `build/examples/cpp/rps-example-client` (or `.exe`) — C++ gRPC example client

The standalone CLI, server, and vstscannermaster all auto-locate `rps-pluginscanner` relative to their own path.

## Usage

RPS can be used in three ways:
1. **Standalone CLI** (`rps-standalone`) — run scans directly from the command line.
2. **gRPC Server** (`rps-server`) — a long-lived daemon that accepts scan requests from any language client. See [gRPC Server](#grpc-server) and [Python TUI Client](#python-tui-client) below.
3. **VST3 Scanner Master** (`vstscannermaster`) — drop-in replacement for Steinberg's `vstscannermaster.exe` that produces Cubase/Nuendo/Dorico-compatible XML cache files. See [VST3 Scanner Master](#vst3-scanner-master) below.

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

## Optional: VST2.4 Support

VST2 scanning is an **opt-in compile-time feature** because the VST2.4 SDK is proprietary and cannot be redistributed. It is disabled by default.

### Enabling VST2

1. **Obtain the VST2.4 SDK** from Steinberg (no longer publicly available — you must have a prior license agreement).
2. **Place the SDK** so that `pluginterfaces/vst2.x/aeffect.h` exists at:
   ```
   libs/rps-core/include/rps/core/vstsdk2.4/pluginterfaces/vst2.x/aeffect.h
   ```
3. **Enable the feature** at configure time:
   ```bash
   # Via CMake flag:
   cmake -G Ninja -DRPS_ENABLE_VST2=ON -B build

   # Or with a custom SDK path:
   cmake -G Ninja -DRPS_ENABLE_VST2=ON -DRPS_VST2_SDK_PATH=/path/to/vstsdk2.4 -B build
   ```
4. **Build normally**: `cmake --build build`

When `RPS_ENABLE_VST2` is OFF (the default), no VST2 SDK code is compiled or linked. The `--formats all` flag **never** includes VST2 — you must explicitly pass `--formats vst2`.

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

`vstscannermaster` is a drop-in replacement for Steinberg's `vstscannermaster.exe`. It uses the rps crash-isolated scanning infrastructure to produce the same XML cache files that Cubase, Nuendo, and Dorico consume.

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

| File | Description |
|---|---|
| `vst3plugins.xml` | All successfully scanned VST3 plugins with full class metadata |
| `vst3blocklist.xml` | Plugins that crashed or timed out during scanning |
| `vst3allowlist.xml` | User-managed allow list (created empty, preserved on subsequent runs) |
| `cacheVersion` | Cache format version |
| `vstscannermaster.log` | Scan log with summary statistics |
| `rps-cache.db` | Internal SQLite database (used for incremental scanning) |

## gRPC Server

`rps-server` exposes the scan engine as a gRPC service, making RPS usable from any language.

### Server CLI

```text
RPS Server Options:
  -h [ --help ]                    Produce help message
  -p [ --port ] arg (=50051)       gRPC listen port
     --db arg (=rps-plugins.db)    Path to the SQLite database file
  -b [ --scanner-bin ] arg         Path to the scanner binary
     --log arg (=rps-server.log)   Log file path
     --log-level arg (=info)       Log level: trace, debug, info, warn, error
```

### gRPC API

Defined in `proto/rps.proto`:

| RPC | Type | Description |
|---|---|---|
| `StartScan` | Server streaming | Start a scan, returns a stream of `ScanEvent` messages until completion |
| `StopScan` | Unary | Stop a running scan gracefully |
| `GetStatus` | Unary | Query server state (idle/scanning), uptime, db path |
| `Shutdown` | Unary | Graceful server shutdown |

Only one scan at a time is allowed. `StartScan` returns `ALREADY_EXISTS` if a scan is in progress.

### Starting the Server

```bash
# Start with defaults (port 50051, rps-plugins.db)
./rps-server

# Custom options
./rps-server --port 50051 --db my-plugins.db --log-level debug
```

## Python TUI Client

A fully functional example client lives in `examples/python/`. It auto-spawns and kills `rps-server`, and displays rich per-worker progress bars using the `rich` library.

### Setup

```bash
cd examples/python
python -m venv .venv
source .venv/bin/activate   # or .venv\Scripts\activate on Windows
pip install -e .

# Generate gRPC stubs (generates and patches imports)
python generate_proto.py
```

### Usage

```bash
# Scan with TUI (auto-spawns server)
python -m rps_client scan --formats vst3,clap --limit 50

# Connect to an already-running server
python -m rps_client --server localhost:50051 scan
python -m rps_client --server localhost:50051 status
```

See `examples/python/README.md` for full documentation.

## C++ Example Client

A C++ gRPC client with an ANSI terminal TUI is built alongside the project as `rps-example-client`.

### Usage

```bash
# Scan with TUI (auto-spawns server)
./rps-example-client scan --formats vst3,clap --limit 50

# Full scan of all formats
./rps-example-client scan --mode full

# Connect to an already-running server
./rps-example-client --server localhost:50051 scan --formats vst3

# Query server status
./rps-example-client --server localhost:50051 status

# Shut down server
./rps-example-client --server localhost:50051 shutdown
```

The binary auto-locates `rps-server` relative to its own build directory. Use `--server-bin` to override.

## Documentation

For developers looking to contribute, understand the architecture, or build the project from source, please read the [Developer Guide](DEVELOPMENT.md).

## License

MIT License
