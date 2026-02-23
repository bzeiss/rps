# RPS - Reliable Plugin Scanner

RPS is a modern, cross-platform audio plugin scanner designed from the ground up for extreme robustness and reliability. It supports scanning VST2, VST3, CLAP, AAX, AU, and LV2 formats on Windows, macOS, and Linux.

## Why RPS?

Scanning audio plugins is notoriously unreliable. Many plugins contain bugs, fail iLok license checks, or enter infinite loops during initialization. When a DAW or host application attempts to scan these plugins directly in its main process, a single bad plugin can crash the entire application.

RPS solves this by using a **multi-process architecture**:
- **`rps-pluginscanorchestrator`**: The coordinator. It manages a pool of worker processes, handles watchdogs/timeouts, and aggregates the results into a central SQLite database. If a plugin crashes, only the worker dies—the orchestrator logs the failure and moves on.
- **`rps-pluginscanner`**: The worker. It isolates the unsafe, third-party plugin code from the rest of your system.

## Project Axioms
1. **Primary Objective**: Robustness (Crash and stall isolation).
2. **Secondary Objective**: Performance / Speed (Parallel scanning).
3. **Third Objective**: Ease of use via central SQLite database (external tools can simply query the DB).
4. **Language**: Strictly Modern C++23.
5. **Dependencies**: Restricted to STL, Boost, SQLite, and Plugin SDKs.

## Building

### Prerequisites

| Dependency | Minimum Version | Notes |
|---|---|---|
| CMake | 3.25 | Build system |
| C++ Compiler | C++23 capable | Clang 16+ (recommended), GCC 13+, MSVC 2022 17.5+ |
| Ninja | 1.11+ | Recommended build backend (optional, can use Make or VS) |
| SQLite3 | 3.x | For the plugin database |
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
          mingw-w64-clang-x86_64-clang mingw-w64-clang-x86_64-sqlite3
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

#### macOS

Install build tools via Homebrew:
```bash
brew install cmake ninja sqlite
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
sudo dnf install cmake ninja-build gcc-c++ clang sqlite-devel git
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
sudo apt install cmake ninja-build g++ clang libsqlite3-dev git
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

After a successful build, you will find two binaries in the `build/` directory:
- `build/apps/rps-pluginscanorchestrator/rps-pluginscanorchestrator` (or `.exe`)
- `build/apps/rps-pluginscanner/rps-pluginscanner` (or `.exe`)

Both binaries must reside in the **same directory** for the orchestrator to auto-locate the scanner worker.

## Usage

You only need to interact with the **Orchestrator**. The orchestrator will automatically spawn the worker scanner processes as needed.

### Command Line Arguments

```text
Orchestrator Options:
  -h [ --help ]                         Produce help message
  -d [ --scan-dir ] arg                 Directories to recursively scan for plugins
  -s [ --scan ] arg                     Single file to scan
  -b [ --scanner-bin ] arg              Path to the scanner binary (default: rps-pluginscanner.exe)
  -t [ --timeout ] arg (=10000)         Timeout in milliseconds for the scanner to respond
  -j [ --jobs ] arg                     Number of parallel workers (default: system CPU core count)
  -f [ --formats ] arg (=all)           Comma-separated list of formats to scan (e.g. vst3,clap) or 'all'
     --filter arg                       Only scan plugins whose filename contains this string
  -l [ --limit ] arg (=0)               Maximum number of plugins to scan (0 = unlimited)
  -v [ --verbose ]                      Enable verbose scanner output (plugin debug logs)
     --db arg (=rps-plugins.db)         Path to the output SQLite database file
```

### Examples

**1. Scan the default OS plugin directories for all formats**
If you run the orchestrator without any path arguments, it will automatically search the standard VST3, CLAP, VST2, AU, and AAX folders for your specific OS.
```bash
./rps-pluginscanorchestrator
```

**2. Scan only VST3 and CLAP formats**
```bash
./rps-pluginscanorchestrator --formats vst3,clap
```

**3. Scan a specific directory**
```bash
# Windows
rps-pluginscanorchestrator.exe --scan-dir "C:\Program Files\Common Files\VST3"

# macOS / Linux
./rps-pluginscanorchestrator --scan-dir "/Library/Audio/Plug-Ins/VST3"
```

**4. Filter plugins by name and limit the count (useful for debugging)**
```bash
rps-pluginscanorchestrator.exe --formats vst3 --filter "FabFilter" --limit 10
```

**5. Scan multiple directories with a specific number of workers**
```bash
rps-pluginscanorchestrator.exe --scan-dir "C:\Folder1" "D:\Folder2" --jobs 4
```

**6. Scan a single plugin with verbose debug output**
```bash
rps-pluginscanorchestrator.exe --scan "C:\VstPlugins\Massive.dll" --verbose
```

**7. Scan with a longer timeout (for slow iLok-protected plugins)**
```bash
rps-pluginscanorchestrator.exe --timeout 30000
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

## Documentation

For developers looking to contribute, understand the architecture, or build the project from source, please read the [Developer Guide](DEVELOPMENT.md).

## License

MIT License
