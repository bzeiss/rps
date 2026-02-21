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
```

### Examples

**1. Scan the default OS plugin directories**
If you run the orchestrator without any path arguments, it will automatically search the standard VST3, CLAP, VST2, AU, and AAX folders for your specific OS.
```bash
./rps-pluginscanorchestrator
```

**2. Scan a specific directory**
```bash
./rps-pluginscanorchestrator --scan-dir "C:\My\Custom\VstPlugins"
```

**3. Scan multiple directories with a specific number of workers**
```bash
./rps-pluginscanorchestrator --scan-dir "C:\Folder1" "D:\Folder2" --jobs 4
```

**4. Scan a single plugin**
```bash
./rps-pluginscanorchestrator --scan "C:\VstPlugins\Massive.dll"
```

### Default Plugin Paths

If no paths are provided, RPS will automatically search the following directories based on your operating system:

**Windows:**
- `C:\Program Files\Common Files\VST3`
- `C:\Program Files (x86)\Common Files\VST3`
- `C:\Program Files\Common Files\CLAP`
- `C:\Program Files (x86)\Common Files\CLAP`
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
